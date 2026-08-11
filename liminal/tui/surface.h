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
    ITALIC,
    MUTED,
    ACCENT,
    CODE,
    CODE_KEYWORD,
    CODE_PREPROCESSOR,
    CODE_TYPE,
    CODE_FUNCTION,
    CODE_STRING,
    CODE_COMMENT,
    CODE_NUMBER,
    CODE_CONSTANT,
    CODE_PROPERTY,
    CODE_OPERATOR,
    LINK,
    DIFF_ADDITION,
    DIFF_DELETION,
    DIFF_HUNK,
    FAILURE,
    COMPOSER,
    FOOTER_MODEL,
    FOOTER_WORKSPACE,
    FOOTER_CONTEXT,
    FOOTER_TOKENS,
};

struct StyledSpan {
    std::string text;
    Style style = Style::NORMAL;

    friend bool operator==(const StyledSpan &, const StyledSpan &) = default;
};

struct Cell {
    std::string text = " ";
    Style style = Style::NORMAL;
    bool continuation = false;
    bool selected = false;

    friend bool operator==(const Cell &, const Cell &) = default;
};

struct Surface {
    Surface(i32 columns = 0, i32 rows = 0);

    void clear();
    void fill_row(i32 row, Style style);
    i32 write(i32 row, i32 column, std::string_view text, Style style = Style::NORMAL);
    std::string row_text(i32 row) const;

    i32 columns = 0;
    i32 rows = 0;
    std::vector<Cell> cells;

    friend bool operator==(const Surface &, const Surface &) = default;
};

struct Cursor {
    i32 row = 0;
    i32 column = 0;
    bool visible = false;

    friend bool operator==(const Cursor &, const Cursor &) = default;
};

struct Frame {
    Surface surface;
    Cursor cursor;

    friend bool operator==(const Frame &, const Frame &) = default;
};

struct GraphemeSpan {
    usize offset = 0;
    usize size = 0;
    i32 width = 0;
};

/// Returns the extended grapheme cluster beginning at or after `offset`.
/// Invalid UTF-8 is represented as one replacement-character cluster per
/// maximal invalid subpart.
GraphemeSpan next_grapheme(std::string_view text, usize offset = 0) noexcept;
usize previous_grapheme_boundary(std::string_view text, usize offset) noexcept;
usize next_grapheme_boundary(std::string_view text, usize offset) noexcept;

/// Exact Unicode 17 terminal-cell width. String measurement is performed per
/// extended grapheme cluster so emoji and joined sequences are not overcounted.
i32 cell_width(char32_t codepoint) noexcept;
i32 text_width(std::string_view text) noexcept;

/// Replaces terminal control characters and invalid UTF-8 with U+FFFD. Plain
/// redirected output may preserve newline, carriage return, and tab as layout
/// controls; frame cell content never does.
std::string sanitize_terminal_text(std::string_view text, bool preserve_layout_controls = false);

/// Encodes a complete frame using cursor-addressed VT output. The encoder
/// always restores a visible cursor when the frame requests one.
std::string encode_frame(const Frame &frame);

/// Encodes only rows and cursor state that differ from `previous`. A missing
/// or differently sized previous frame falls back to a complete frame.
std::string encode_frame_diff(const Frame *previous, const Frame &frame);

} // namespace liminal::tui
