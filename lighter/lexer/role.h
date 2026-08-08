#pragma once

#include <array>
#include <concepts>
#include <meta>
#include <type_traits>
#include <utility>

#include <lighter/types.hpp>

namespace lighter::lexer {

enum struct TokenRole : u8 {
    DEFAULT,
    COMMENT,
    DOCUMENTATION,
    KEYWORD,
    TYPE,
    FUNCTION,
    IDENTIFIER,
    PARAMETER,
    PROPERTY,
    CONSTANT,
    STRING,
    CHARACTER,
    ESCAPE,
    NUMBER,
    OPERATOR,
    PREPROCESSOR,
    ATTRIBUTE,
    LABEL,
    MODULE,
    UNRECOGNIZED,
};

/// Explicit semantic role attached to a lexer-local style enumerator.
///
/// enum struct Style : u8 {
///     DEFAULT [[=token_role(TokenRole::DEFAULT)]] = 0,
///     COMMENT [[=token_role(TokenRole::COMMENT)]],
/// };
struct StyleRole {
    TokenRole value = TokenRole::DEFAULT;

    constexpr StyleRole operator()(TokenRole role) const noexcept { return {.value = role}; }
};

inline constexpr StyleRole token_role{};

namespace detail {

template <typename Style>
consteval std::array<TokenRole, 256> make_style_role_table() {
    static_assert(std::is_enum_v<Style>);
    static_assert(std::same_as<std::underlying_type_t<Style>, u8>);

    std::array<TokenRole, 256> result{};
    std::array<bool, 256> occupied{};
    result.fill(TokenRole::UNRECOGNIZED);

    for (auto enumerator : std::meta::enumerators_of(^^Style)) {
        const auto index = static_cast<usize>(std::to_underlying(std::meta::extract<Style>(enumerator)));
        if (occupied[index]) {
            throw "lexer style IDs must be unique";
        }

        bool found_role = false;
        for (auto annotation : std::meta::annotations_of(enumerator)) {
            if (std::meta::remove_cv(std::meta::type_of(annotation)) != ^^StyleRole) {
                continue;
            }
            if (found_role) {
                throw "lexer styles must have exactly one token role";
            }
            result[index] = std::meta::extract<StyleRole>(annotation).value;
            found_role = true;
        }
        if (!found_role) {
            throw "lexer styles must have a token role";
        }
        occupied[index] = true;
    }

    if (!occupied[0] || result[0] != TokenRole::DEFAULT) {
        throw "lexer style zero must exist and have the default token role";
    }
    return result;
}

} // namespace detail

template <typename Style>
    requires std::is_enum_v<Style>
inline constexpr auto style_role_table = detail::make_style_role_table<Style>();

template <typename Style>
    requires std::is_enum_v<Style>
[[nodiscard]] constexpr TokenRole role_for_style(Style style) noexcept {
    return style_role_table<Style>[static_cast<u8>(style)];
}

template <typename Style>
    requires std::is_enum_v<Style>
[[nodiscard]] constexpr TokenRole role_for_style(u8 style) noexcept {
    return style_role_table<Style>[style];
}

} // namespace lighter::lexer
