
#include <iostream>

#include "define_enum.hpp"

/* 1) List every enumerator once, in an X‑macro */
#define COLOUR_ENUM(X) \
        X(red)         \
        X(green)       \
        X(blue)

/* 2) Expand that list through the helper */
DEFINE_ENUM_WITH_STRING_CONVERSIONS(Colour, COLOUR_ENUM)

#include "Colour.hpp"

int main() {
    Colour c = Colour::green;

    std::cout << c << '\n';                   // → "green"
    std::string_view name = to_string(c);     // → "green"

    if (auto parsed = Colour_from_string("blue"))
        std::cout << "Got enum value "
                  << *parsed << '\n';         // → "Got enum value blue"
}
