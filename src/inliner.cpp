//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "inliner.h"
#include "clang_version.h"
#include "util.h"

#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sstream>



using namespace clang;
using std::set;
using std::string;
using std::vector;

namespace caide {
namespace internal {

struct IncludeReplacement {
    SourceRange includeDirectiveRange;
    string fileName;
    string replaceWith;
};

class TrackMacro: public PPCallbacks {
public:
    TrackMacro(SourceManager& srcManager_, set<string>& includedHeaders_,
               vector<IncludeReplacement>& replacements_)
        : srcManager(srcManager_)
        , includedHeaders(includedHeaders_)
        , replacementStack(replacements_)
    {
        // Setup a placeholder where the result for the whole CPP file will be stored
        replacementStack.resize(1);
        replacementStack.back().fileName = "<CPP>";
    }

    virtual void InclusionDirective(SourceLocation HashLoc,
                                    const Token& /*IncludeTok*/,
                                    StringRef FileName,
                                    bool /*IsAngled*/,
                                    CharSourceRange FilenameRange,
                                    const FileEntry *File,
                                    StringRef /*SearchPath*/,
                                    StringRef /*RelativePath*/,
                                    const Module* /*Imported*/
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
                                    , SrcMgr::CharacteristicKind /*FileType*/
#endif
                                    ) override
    {
        if (FileName.empty())
            return;
        // Don't track system headers including each other
        // They may include the same file multiple times (no include guards) and do other crazy stuff
        if (!isUserFile(HashLoc))
            return;

        // Inclusion directive is encountered.
        // Setup a placeholder in inclusion stack where the result of this
        // directive will be stored. Initially assume the directive remains unchanged
        // (this is the case if it's a new system header).
        SourceLocation end = FilenameRange.getEnd();
        const char* s = srcManager.getCharacterData(HashLoc);
        const char* e = srcManager.getCharacterData(end);

        if (!File) {
            //std::cerr << "Compilation error: " << FileName.str() << " not found\n";
            return;
        }

        IncludeReplacement rep;
        rep.includeDirectiveRange = SourceRange(HashLoc, end);
        rep.fileName = getCanonicalPath(srcManager.getFileEntryForID(srcManager.getFileID(HashLoc)));
        if (s && e)
            rep.replaceWith = string(s, e);
        else
            rep.replaceWith = "<Inliner error>\n";
        replacementStack.push_back(rep);
    }

    virtual void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                             SrcMgr::CharacteristicKind /*FileType*/,
                             FileID PrevFID/* = FileID()*/) override
    {
        const FileEntry* curEntry = srcManager.getFileEntryForID(PrevFID);
        if (Reason == PPCallbacks::ExitFile && curEntry) {
            // Don't track system headers including each other
            if (!isUserFile(Loc))
                return;
            // Rewind replacement stack and compute result of including current file.
            string currentFile = getCanonicalPath(curEntry);

            // - Search the stack for the topmost replacement belonging to another file.
            //   That's where we were included from.
            int includedFrom = int(replacementStack.size()) - 1;
            while (replacementStack[includedFrom].fileName == currentFile)
                --includedFrom;

            // - Mark this header as visited for future CPP files.
            if (!markAsIncluded(currentFile)) {
                // - If current header should be skipped, set empty replacement
                replacementStack[includedFrom].replaceWith = "";
            } else if (isSystemHeader(PrevFID)) {
                // - This is a new system header. Leave include directive as is,
                //   i. e. do nothing.
            } else {
                // - This is a new user header. Apply all replacements from current file.
                replacementStack[includedFrom].replaceWith = calcReplacements(includedFrom, PrevFID);
            }

            // - Actually rewind.
            replacementStack.resize(includedFrom + 1);
        }
    }

    virtual void EndOfMainFile() override {
        replacementStack[0].replaceWith = calcReplacements(0, srcManager.getMainFileID());
        replacementStack.resize(1);
    }

#if CAIDE_CLANG_VERSION_AT_LEAST(10, 0)
    virtual void FileSkipped(const FileEntryRef& SkippedFileRef, const Token &FilenameTok,
                             SrcMgr::CharacteristicKind /*FileType*/) override
    {
        const FileEntry& SkippedFile = SkippedFileRef.getFileEntry();
#else
    virtual void FileSkipped(const FileEntry& SkippedFile, const Token &FilenameTok,
                             SrcMgr::CharacteristicKind /*FileType*/) override
    {
#endif
        // Don't track system headers including each other
        if (!srcManager.isInSystemHeader(FilenameTok.getLocation())) {
            // File skipped as part of normal header guard optimization / #pragma once
            //
            // It's important to do a manual check here because in other versions of STL
            // the header may not have been included. In other words, we need to explicitly
            // include every file that we use.
            if (!markAsIncluded(SkippedFile))
                replacementStack.back().replaceWith = "";
        }
    }

    string getResult() const {
        if (replacementStack.size() != 1)
            return "C++ inliner error";
        else
            return replacementStack[0].replaceWith;
    }

    virtual ~TrackMacro() override = default;

private:
    SourceManager& srcManager;

    /*
     * Headers that have been included explicitly by user code (i.e. from a cpp file or from
     * a non-system header).
     */
    set<string>& includedHeaders;

    /*
     * A 'stack' of replacements, reflecting current include stack.
     * Replacements in the same file are ordered by their location.
     * Replacement string may be empty which means that we skip this include file.
     */
    vector<IncludeReplacement>& replacementStack;

private:

    /*
     * Unwinds inclusion stack and calculates the result of inclusion of current file
     */
    string calcReplacements(int includedFrom, FileID currentFID) const {
        std::ostringstream result;
        // We go over each #include directive in current file and replace it
        // with the result of inclusion.
        // The last value of i doesn't correspond to an include directive,
        // it's used to output the part of the file after the last include directive.
        for (int i = includedFrom + 1; i <= int(replacementStack.size()); ++i) {
            // First output the block before the #include directive.
            // Block start is immediately after the previous include directive;
            // block end is immediately before current include directive.
            SourceLocation blockStart, blockEnd;

            if (i == includedFrom + 1)
                blockStart = srcManager.getLocForStartOfFile(currentFID);
            else
                blockStart = replacementStack[i-1].includeDirectiveRange.getEnd();

            if (i == int(replacementStack.size()))
                blockEnd = srcManager.getLocForEndOfFile(currentFID);
            else
                blockEnd = replacementStack[i].includeDirectiveRange.getBegin();

            // skip cases when two include directives are adjacent
            //   or an include directive is in the beginning or end of file
            if (blockStart.isValid() && blockEnd.isValid() &&
                    srcManager.isBeforeInSLocAddrSpace(blockStart, blockEnd)) {
                bool invalid;
                const char* b = srcManager.getCharacterData(blockStart, &invalid);
                const char* e = 0;
                if (!invalid)
                    e = srcManager.getCharacterData(blockEnd, &invalid);
                if (invalid || !b || !e)
                    result << "<Inliner error>\n";
                else
                    result << string(b, e);
            }

            // Now output the result of file inclusion
            if (i != int(replacementStack.size()))
                result << replacementStack[i].replaceWith;
        }
        return result.str();
    }

    string getCanonicalPath(const FileEntry* entry) const {
        const DirectoryEntry* dirEntry = entry->getDir();
        StringRef strRef = srcManager.getFileManager().getCanonicalName(dirEntry);
        string res = strRef.str();
        res.push_back('/');
        string fname(entry->getName());
        int i = (int)fname.size() - 1;
        while (i >= 0 && fname[i] != '/' && fname[i] != '\\')
            --i;
        res += fname.substr(i+1);
        return res;
    }

    bool markAsIncluded(const FileEntry& entry) {
        string fname = getCanonicalPath(&entry);
        return markAsIncluded(fname);
    }

    bool markAsIncluded(const string& canonicalPath) {
        return includedHeaders.insert(canonicalPath).second;
    }

    bool isSystemHeader(FileID header) const {
        SourceLocation loc = srcManager.getLocForStartOfFile(header);
        return srcManager.isInSystemHeader(loc);
    }

    bool isUserFile(SourceLocation loc) const {
        return !srcManager.isInSystemHeader(loc) && loc.isValid();
    }

    void debug() const {
        for (size_t i = 0; i < replacementStack.size(); ++i) {
            std::cerr << replacementStack[i].fileName << " " <<
                         replacementStack[i].replaceWith << "\n";
        }
    }
};

class InlinerFrontendAction : public PreprocessOnlyAction {
private:
    vector<IncludeReplacement>& replacementStack;
    set<string>& includedHeaders;

public:
    InlinerFrontendAction(vector<IncludeReplacement>& _replacementStack,
                          set<string>& _includedHeaders)
        : replacementStack(_replacementStack)
        , includedHeaders(_includedHeaders)
    {}

    bool BeginSourceFileAction(CompilerInstance& compiler) override {
        compiler.getPreprocessor().addPPCallbacks(std::unique_ptr<TrackMacro>(new TrackMacro(
                compiler.getSourceManager(), includedHeaders, replacementStack)));
        return true;
    }
};

class InlinerFrontendActionFactory: public tooling::FrontendActionFactory {
private:
    vector<IncludeReplacement>& replacementStack;
    set<string>& includedHeaders;

public:
    InlinerFrontendActionFactory(vector<IncludeReplacement>& replacementStack_,
                                 set<string>& includedHeaders_)
        : replacementStack(replacementStack_)
        , includedHeaders(includedHeaders_)
    {}
#if CAIDE_CLANG_VERSION_AT_LEAST(10, 0)
    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<InlinerFrontendAction>(replacementStack, includedHeaders);
    }
#else
    FrontendAction* create() override {
        return new InlinerFrontendAction(replacementStack, includedHeaders);
    }
#endif
};

Inliner::Inliner(const vector<string>& cmdLineOptions_)
    : cmdLineOptions(cmdLineOptions_)
{}

string Inliner::doInline(const string& cppFile) {
    std::unique_ptr<clang::tooling::FixedCompilationDatabase> compilationDatabase(
        createCompilationDatabaseFromCommandLine(cmdLineOptions));

    vector<string> sources(1);
    sources[0] = cppFile;

    vector<IncludeReplacement> replacementStack;
    InlinerFrontendActionFactory factory(replacementStack, includedHeaders);

    clang::tooling::ClangTool tool(*compilationDatabase, sources);

    int ret = tool.run(&factory);

    if (ret != 0)
        throw std::runtime_error("Compilation error");
    else if (replacementStack.size() != 1)
        throw std::logic_error("Caide inliner error");

    inlineResults.push_back(replacementStack[0].replaceWith);
    return inlineResults.back();
}

}
}

