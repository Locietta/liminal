#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

namespace lighter::lexer {

struct LexRange {
    usize begin = 0;
    usize end = 0;
};

/// Contiguous storage shared by every language lexer. Styles are lexer-local
/// IDs; line states are opaque checkpoints interpreted only by that lexer.
struct Document {
    std::string source;
    std::vector<u8> styles;
    std::vector<usize> line_starts{0};
    std::vector<u32> line_states{0};
};

struct LexContext {
    std::string_view source;
    std::span<u8> styles;
    std::span<const usize> line_starts;
    std::span<u32> line_states;
    LexRange range;
};

void assign(Document &document, std::string_view source);

/// Appends streamed source and returns the range that must be re-lexed. The
/// range starts at the beginning of the previously unfinished last line.
[[nodiscard]] LexRange append(Document &document, std::string_view source);

[[nodiscard]] usize line_from_position(const Document &document, usize position) pre(position <= document.source.size());

[[nodiscard]] LexContext context(Document &document, LexRange range) pre(range.begin <= range.end) pre(range.end <= document.source.size());

} // namespace lighter::lexer
