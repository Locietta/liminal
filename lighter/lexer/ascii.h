#pragma once

namespace lighter::lexer {

[[nodiscard]] constexpr bool ascii_digit(char value) noexcept { return value >= '0' && value <= '9'; }

[[nodiscard]] constexpr bool ascii_lower(char value) noexcept { return value >= 'a' && value <= 'z'; }

[[nodiscard]] constexpr bool ascii_upper(char value) noexcept { return value >= 'A' && value <= 'Z'; }

[[nodiscard]] constexpr bool ascii_alpha(char value) noexcept { return ascii_lower(value) || ascii_upper(value); }

[[nodiscard]] constexpr bool ascii_alphanumeric(char value) noexcept { return ascii_alpha(value) || ascii_digit(value); }

[[nodiscard]] constexpr bool ascii_hex_digit(char value) noexcept {
    return ascii_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

[[nodiscard]] constexpr bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

[[nodiscard]] constexpr bool ascii_identifier_start(char value) noexcept {
    return ascii_alpha(value) || value == '_' || static_cast<unsigned char>(value) >= 0x80;
}

[[nodiscard]] constexpr bool ascii_identifier_continue(char value) noexcept {
    return ascii_alphanumeric(value) || value == '_' || static_cast<unsigned char>(value) >= 0x80;
}

[[nodiscard]] constexpr char ascii_to_lower(char value) noexcept {
    return ascii_upper(value) ? static_cast<char>(value + ('a' - 'A')) : value;
}

} // namespace lighter::lexer
