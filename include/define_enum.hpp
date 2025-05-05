#pragma once
/**
 * @file enum_utils.hpp
 * @brief Utilities for declaring strongly‑typed enums with automatic string conversion.
 *
 * The single public macro ::DEFINE_ENUM_WITH_STRING_CONVERSIONS generates:
 *  - a scoped enum (`enum class EnumName`)
 *  - a compile‑time array with the enumerator spellings
 *  - `to_string(EnumName)`            — enum → `std::string_view`
 *  - `EnumName_from_string(std::string_view)` — string → `EnumName`
 *  - `operator<<(std::ostream&, EnumName)`    — stream insertion
 *
 * All generated code is header‑only, requires only the C++17 standard
 * library, and adds zero runtime overhead in most optimisation levels.
 *
 * @par Usage
 * @code
 *  #define COLOUR_ENUM(X) \
 *      X(red)             \
 *      X(green)           \
 *      X(blue)
 *
 *  DEFINE_ENUM_WITH_STRING_CONVERSIONS(Colour, COLOUR_ENUM)
 * @endcode
 */

#include <array>
#include <string_view>
#include <ostream>

/**
 * @brief Expands to an enumerator inside the generated enum.
 *
 * Used internally by ::DEFINE_ENUM_WITH_STRING_CONVERSIONS.
 */
#define EU_ENUM_DECLARE(name)       name,

/**
 * @brief Expands to a string literal with the enumerator name.
 *
 * Used internally by ::DEFINE_ENUM_WITH_STRING_CONVERSIONS.
 */
#define EU_ENUM_STRING(name)        #name,

// ———————————————————————————————————————————————————————
//  Public macro: DEFINE_ENUM_WITH_STRING_CONVERSIONS
//
//  @param EnumName   The identifier to give the generated scoped enum.
//  @param ENUM_DEF   An X‑macro that lists each enumerator once.
//
//  Example:
//
/**
 * @code
 *  #define FRUIT_ENUM(X) \
 *      X(apple)          \
 *      X(orange)         \
 *      X(banana)
 *
 *  DEFINE_ENUM_WITH_STRING_CONVERSIONS(Fruit, FRUIT_ENUM)
 * @endcode
 */
// ———————————————————————————————————————————————————————
#define DEFINE_ENUM_WITH_STRING_CONVERSIONS(EnumName, ENUM_DEF)                    \
                                                                                    \
    /**                                                                             \
     * @brief Automatically generated scoped enum containing the enumerators.       \
     */                                                                             \
    enum class EnumName {                                                           \
        ENUM_DEF(EU_ENUM_DECLARE)                                                   \
        TOTAL                                                                      \
    };                                                                              \
                                                                                    \
    /**                                                                             \
     * @brief Compile‑time lookup table of enumerator spellings.                    \
     *                                                                              \
     * The position of each string matches the integral value of the corresponding  \
     * enumerator, enabling O(1) conversion in ::to_string.                          \
     */                                                                             \
    inline constexpr std::array<std::string_view,                                   \
        static_cast<std::size_t>(EnumName::TOTAL)>                                 \
        EnumName##_names = {                                                        \
            ENUM_DEF(EU_ENUM_STRING)                                                \
    };                                                                              \
                                                                                    \
    /**                                                                             \
     * @brief Converts an @p EnumName value to its textual representation.          \
     *                                                                              \
     * @param value Enumerated value to convert.                                     \
     * @return A `std::string_view` containing the enumerator name.                 \
     */                                                                             \
    constexpr std::string_view to_string(EnumName value) noexcept {                 \
        return EnumName##_names[static_cast<std::size_t>(value)];                   \
    }                                                                               \
                                                                                    \
    /**                                                                             \
     * @brief Parses a string into the corresponding @p EnumName value.             \
     *                                                                              \
     * Performs a linear search over the compile‑time array of names. For small     \
     * enums the compiler typically unrolls this loop.                              \
     *                                                                              \
     * @param sv String to parse — must exactly match an enumerator spelling.        \
     * @return Enum corresponding to the string (asserts if not present)            \
     */                                                                             \
    inline EnumName EnumName##_from_string(std::string_view sv) {    \
        for (std::size_t i = 0; i < EnumName##_names.size(); ++i)                   \
            if (EnumName##_names[i] == sv)                                          \
                return static_cast<EnumName>(i);                                    \
        assert(false && "Invalid Enum name");                                       \
    }                                                                               \
                                                                                    \
    /**                                                                             \
     * @brief Stream insertion operator for @p EnumName.                            \
     *                                                                              \
     * Enables `std::ostream << EnumName` in logging or debugging code.             \
     *                                                                              \
     * @param os     Output stream.                                                 \
     * @param value  Enumerated value to write.                                     \
     * @return The same stream reference, allowing chaining.                        \
     */                                                                             \
    inline std::ostream& operator<<(std::ostream& os, EnumName value) {             \
        return os << to_string(value);                                              \
    }
