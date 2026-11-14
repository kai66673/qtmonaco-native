// qtmonaco-native C++ highlighting demo — open with:
//   ./build/src/app/qmn-spike examples/demo.cpp
/* The upstream v0.23.4 query colors almost nothing of this: comments,
   numbers, plain strings, primitive types and control keywords come from
   the documented append block (third_party/tree-sitter/VENDORED.md). */

#include <string>      // system_lib_string gets String ink (append block)
#include <vector>      // #pragma/#error/#undef: NOT tokens of this pin — never
                       // put one in a fixture, the parse would break.

#ifndef DEMO_H_        // preproc_ifdef: folds like a block, keyword ink
#define DEMO_H_        // plain #define name -> ALL_CAPS Constant role

#include <cstdio>

namespace geo {

constexpr double PI = 3.14159;   // constexpr: upstream keyword
const int MAX_RAYS = 64;         // the ALL_CAPS rule: Constant, not keyword

class Circle {
public:
    explicit Circle(double r) : r_(r), tag("circle") {}   // field_initializer_list folds
    ~Circle() { }                // destructor_name: function ink (added pattern)

    double area() const {
        if (r_ <= 0.0) return 0.0;   // if/return/<=: append block ink
        for (int i = 0; i < MAX_RAYS; ++i)
            r_ += 0;                 // plain classic for: range-for is not parsed
        return PI * r_ * r_;         // true/false/nullptr are Constants
    }

    bool valid() const { return r_ > 0.0 && r_ < 1e9; }

private:
    double r_;
    std::string tag;               // "std" fails the upstream ^[A-Z] type rule
};                                  // class_specifier: fold + keyword (upstream)

} // namespace geo

template <typename T>                  // template/typename: upstream keywords
T clamp(T v, T lo, T hi) {             // bare-name declarator -> Function (added)
    return v < lo ? lo : (v > hi ? hi : v);
}

int main() {
    using geo::Circle;                 // using: upstream keyword
    Circle unit(1.0), flat(-2.0);
    auto ring = unit;                  // (auto) @type — upstream
    ring = flat;                       // lowercase names: catch-all @variable

    const char* msg = "area\t\"quoted\"\n";   // escape_sequence wins its units
    char septum = '/';                        // char_literal: Number ink
    const char* raw = R"(no \escapes here)";  // raw_string_literal (upstream)
    long big = 1'000'000, bits = 0xFFu, half = .5e-1;

    if (unit.valid() && !flat.valid())        // call: obj.method -> Function ink
        printf("%s %.3f\n", msg, unit.area());
    else if (clamp(3, 1, 2) == 2)
        puts("clamped");
    else
        return 1;

    try {                                     // try/catch: upstream keywords
        FILE* f = nullptr;                    // nullptr: upstream @constant
        if (f == nullptr) throw int{1};       // throw/new/delete: upstream ink
    } catch (int code) {
        (void)code;
    }

    return 0;
}

#endif // DEMO_H_
