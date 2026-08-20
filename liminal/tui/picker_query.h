#pragma once

#include <string>
#include <string_view>

#include <lighter/types.hpp>

namespace liminal::tui {

using namespace lighter::types;

/// Editing operations on a picker-owned query. INSERT carries text; every
/// other operation acts at the query cursor.
enum struct PickerQueryEdit {
    INSERT,
    BACKSPACE,
    ERASE,
    BACKSPACE_WORD,
    ERASE_WORD,
    LEFT,
    RIGHT,
    WORD_LEFT,
    WORD_RIGHT,
    HOME,
    END,
};

struct PickerQueryWindow {
    std::string_view text;
    i32 cursor_column = 0;
};

/// Applies one grapheme-aware query edit. Returns true only when the query
/// text changed; cursor-only movement returns false.
bool edit_picker_query(std::string &query, usize &cursor, PickerQueryEdit edit, std::string_view text = {});

/// Returns the cell-bounded grapheme window that keeps the query cursor
/// visible without splitting an extended grapheme cluster.
PickerQueryWindow picker_query_window(std::string_view query, usize cursor, i32 available_cells);

std::string ascii_fold(std::string_view text);
bool ascii_case_insensitive_contains(std::string_view text, std::string_view query);

} // namespace liminal::tui
