#include "picker_query.h"

#include <algorithm>

#include <liminal/tui/surface.h>

namespace liminal::tui {

bool edit_picker_query(std::string &query, usize &cursor, PickerQueryEdit edit, std::string_view text) {
    cursor = std::min(cursor, query.size());
    switch (edit) {
        case PickerQueryEdit::LEFT: cursor = previous_grapheme_boundary(query, cursor); return false;
        case PickerQueryEdit::RIGHT: cursor = next_grapheme_boundary(query, cursor); return false;
        case PickerQueryEdit::WORD_LEFT: cursor = previous_word_boundary(query, cursor); return false;
        case PickerQueryEdit::WORD_RIGHT: cursor = next_word_boundary(query, cursor); return false;
        case PickerQueryEdit::HOME: cursor = 0; return false;
        case PickerQueryEdit::END: cursor = query.size(); return false;
        case PickerQueryEdit::INSERT:
            if (text.empty()) return false;
            {
                std::string printable;
                printable.reserve(text.size());
                for (const auto character : text) {
                    const auto byte = static_cast<unsigned char>(character);
                    if (byte >= 0x20 && byte != 0x7f) printable += character;
                }
                if (printable.empty()) return false;
                query.insert(cursor, printable);
                cursor += printable.size();
            }
            return true;
        case PickerQueryEdit::BACKSPACE: {
            const auto boundary = previous_grapheme_boundary(query, cursor);
            if (boundary == cursor) return false;
            query.erase(boundary, cursor - boundary);
            cursor = boundary;
            return true;
        }
        case PickerQueryEdit::ERASE: {
            const auto boundary = next_grapheme_boundary(query, cursor);
            if (boundary == cursor) return false;
            query.erase(cursor, boundary - cursor);
            return true;
        }
        case PickerQueryEdit::BACKSPACE_WORD: {
            const auto boundary = previous_word_boundary(query, cursor);
            if (boundary == cursor) return false;
            query.erase(boundary, cursor - boundary);
            cursor = boundary;
            return true;
        }
        case PickerQueryEdit::ERASE_WORD: {
            const auto boundary = next_word_boundary(query, cursor);
            if (boundary == cursor) return false;
            query.erase(cursor, boundary - cursor);
            return true;
        }
    }
    return false;
}

PickerQueryWindow picker_query_window(std::string_view query, usize cursor, i32 available_cells) {
    if (available_cells <= 0) return {};

    cursor = std::min(cursor, query.size());
    usize start = 0;
    auto cursor_column = text_width(query.substr(0, cursor));
    while (start < cursor && cursor_column >= available_cells) {
        const auto grapheme = next_grapheme(query, start);
        start += grapheme.size;
        cursor_column -= grapheme.width;
    }

    auto end = start;
    i32 used = 0;
    while (end < query.size()) {
        const auto grapheme = next_grapheme(query, end);
        if (used + grapheme.width > available_cells) break;
        used += grapheme.width;
        end += grapheme.size;
    }
    return {.text = query.substr(start, end - start), .cursor_column = cursor_column};
}

std::string ascii_fold(std::string_view text) {
    std::string folded(text);
    for (auto &character : folded) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return folded;
}

bool ascii_case_insensitive_contains(std::string_view text, std::string_view query) { return ascii_fold(text).contains(ascii_fold(query)); }

} // namespace liminal::tui
