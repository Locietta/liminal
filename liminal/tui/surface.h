#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

namespace liminal::tui {

using namespace lighter::types;

enum struct Style {
    NORMAL,
    EMPHASIS,
    MUTED,
    ACCENT,
    FAILURE,
};

struct Cell {
    std::string text = " ";
    Style style = Style::NORMAL;
    bool continuation = false;
};

struct Surface {
    Surface(i32 columns = 0, i32 rows = 0);

    void clear();
    i32 write(i32 row, i32 column, std::string_view text, Style style = Style::NORMAL);
    std::string row_text(i32 row) const;

    i32 columns = 0;
    i32 rows = 0;
    std::vector<Cell> cells;
};

struct Cursor {
    i32 row = 0;
    i32 column = 0;
    bool visible = false;
};

struct Frame {
    Surface surface;
    Cursor cursor;
};

/// Approximate terminal-cell width for one Unicode scalar. Grapheme-cluster
/// segmentation remains a Phase 1 dependency decision, but combining marks
/// and common wide/emoji ranges already occupy the expected cells.
i32 cell_width(char32_t codepoint) noexcept;
i32 text_width(std::string_view text) noexcept;

/// Encodes a complete frame using cursor-addressed VT output. The encoder
/// always restores a visible cursor when the frame requests one.
std::string encode_frame(const Frame &frame);

} // namespace liminal::tui
