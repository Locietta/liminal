#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include <lighter/types.hpp>

namespace lighter::lexer {

/// Immutable sorted keyword set. Language tables stay visible as ordinary
/// constexpr data and pay no allocation or initialization cost.
template <usize Size>
struct WordSet {
    std::array<std::string_view, Size> words;

    consteval explicit WordSet(std::array<std::string_view, Size> values) : words(values) {
        for (usize index = 1; index < words.size(); ++index) {
            if (words[index - 1] >= words[index]) {
                throw "lexer word sets must be sorted and unique";
            }
        }
    }

    [[nodiscard]] constexpr bool contains(std::string_view word) const noexcept { return std::ranges::binary_search(words, word); }
};

template <usize... Sizes>
[[nodiscard]] consteval auto make_word_set(const char (&...words)[Sizes]) {
    return WordSet<sizeof...(words)>({std::string_view(words, Sizes - 1)...});
}

} // namespace lighter::lexer
