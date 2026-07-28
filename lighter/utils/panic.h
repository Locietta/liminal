#pragma once

#include <source_location>
#include <string_view>

namespace lighter {

[[noreturn]] void panic(std::string_view message, std::source_location location = std::source_location::current()) noexcept;

inline void check(bool condition, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
    if (!condition) [[unlikely]] {
        panic(message, location);
    }
}

} // namespace lighter
