#include <myvector>


namespace spcppl {

    template <typename T>
    double f(T& t) {
        return double(t);
    }

} // namespace spcppl

class X {
public:
    void f() const {
    }
};

template <typename T>
void goF(const T& t) {
    t.f();
}

class Vertex {
public:
    Vertex() {}
};

struct Y {
    bool operator ++ (int x) {
        return true;
    }
};

bool operator + (Y a, Y b) {
    return true;
}

struct Used {
    int x;

    ~Used() {
        x = 2;
    }
};


int main() {
    {
        int x;
        spcppl::f(x);
        goF(X());
    }

    {
        mystd::vector<Vertex> v;
        v.resize(100);
    }

    {
        Y y;
        y++;
        y + y;
    }

    {
        Used used;
    }

    return 0;
};




