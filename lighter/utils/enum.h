#pragma once

#include <meta>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <lighter/types.hpp>

namespace lighter {

/// Rendering styles for enum_name. Conversions go through word splitting,
/// so any identifier in one of these three styles converts to any other.
enum struct NameCase {
    NO_CHANGE, // identifier verbatim
    CAMEL,     // MutexWaiter
    LOWER,     // mutex_waiter
    UPPER,     // MUTEX_WAITER
};

namespace detail::name_case {

constexpr bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr char to_upper(char c) { return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c; }
constexpr char to_lower(char c) { return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }

/// Split an identifier into words. Boundaries: '_', a lower->upper
/// transition (camelCase), and the last upper of an upper run followed by a
/// lower (acronyms: "HTTPServer" -> "HTTP", "Server").
consteval std::vector<std::string> split_words(std::string_view name) {
    std::vector<std::string> words;
    std::string current;
    for (usize i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }

        const bool camel_boundary = !current.empty() && is_upper(c) && is_lower(current.back());
        const bool acronym_boundary =
            !current.empty() && is_upper(current.back()) && is_upper(c) && i + 1 < name.size() && is_lower(name[i + 1]);
        if (camel_boundary || acronym_boundary) {
            words.push_back(current);
            current.clear();
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

consteval std::string render(std::string_view name, NameCase to) {
    if (to == NameCase::NO_CHANGE) {
        return std::string(name);
    }

    std::string out;
    bool first = true;
    for (const auto &word : split_words(name)) {
        switch (to) {
            case NameCase::NO_CHANGE: break; // handled above
            case NameCase::CAMEL:
                for (usize i = 0; i < word.size(); ++i) {
                    out.push_back(i == 0 ? to_upper(word[i]) : to_lower(word[i]));
                }
                break;
            case NameCase::LOWER:
                if (!first) {
                    out.push_back('_');
                }
                for (char c : word) {
                    out.push_back(to_lower(c));
                }
                break;
            case NameCase::UPPER:
                if (!first) {
                    out.push_back('_');
                }
                for (char c : word) {
                    out.push_back(to_upper(c));
                }
                break;
        }
        first = false;
    }
    return out;
}

} // namespace detail::name_case

/// Name of an enumerator, derived from the enum definition via reflection,
/// optionally re-rendered in a requested style (converted names are promoted
/// to static storage at compile time; NO_CHANGE returns the identifier
/// verbatim). Returns `fallback` - unconverted - for values that match no
/// enumerator (out-of-range casts, or combined flag bits). For
/// duplicate-valued enumerators the first declared name wins.
template <NameCase To = NameCase::NO_CHANGE, typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(E value, std::string_view fallback = "Unknown") {
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (value == [:e:]) {
            if constexpr (To == NameCase::NO_CHANGE) {
                return std::meta::identifier_of(e);
            } else {
                return std::define_static_string(detail::name_case::render(std::meta::identifier_of(e), To));
            }
        }
    }
    return fallback;
}

} // namespace lighter
