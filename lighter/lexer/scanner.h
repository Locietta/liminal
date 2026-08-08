#pragma once

#include <algorithm>
#include <concepts>
#include <string_view>
#include <type_traits>

#include <lighter/lexer/document.h>

namespace lighter::lexer {

/// Bounds-safe byte scanner over one lexing range. Looking outside the source
/// returns a zero sentinel, which keeps hot lexer loops branch-light without
/// exposing storage sentinels in Document.
struct Scanner {
    std::string_view source;
    usize position = 0;
    usize end = 0;

    [[nodiscard]] bool done() const noexcept { return position >= end; }

    [[nodiscard]] char peek(isize relative = 0) const noexcept {
        if (relative < 0) {
            const usize distance = static_cast<usize>(-relative);
            if (distance > position) return '\0';
            return source[position - distance];
        }
        const usize index = position + static_cast<usize>(relative);
        return index < source.size() ? source[index] : '\0';
    }

    [[nodiscard]] bool match(std::string_view value) const noexcept { return source.substr(position).starts_with(value); }

    void advance(usize count = 1) noexcept pre(count <= end - position) { position += count; }

    template <typename Predicate>
        requires std::predicate<Predicate, char>
    [[nodiscard]] LexRange take_while(Predicate predicate) {
        const usize begin = position;
        while (!done() && predicate(peek())) {
            advance();
        }
        return {.begin = begin, .end = position};
    }
};

[[nodiscard]] inline Scanner scanner(const LexContext &context) noexcept {
    return {.source = context.source, .position = context.range.begin, .end = context.range.end};
}

template <typename Style>
    requires std::is_enum_v<Style> && std::same_as<std::underlying_type_t<Style>, u8>
void paint(LexContext &context, LexRange range, Style style) pre(range.begin <= range.end) pre(range.end <= context.styles.size()) {
    std::ranges::fill(context.styles.subspan(range.begin, range.end - range.begin), static_cast<u8>(style));
}

} // namespace lighter::lexer
