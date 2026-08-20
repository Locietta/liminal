#include "compact_picker.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include <lighter/utils/panic.h>

namespace liminal::tui {

namespace {

constexpr usize k_max_result_rows = 8;

std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    for (auto &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

bool matches(const CompactPickerItem &item, std::string_view needle, CompactPickerMatch match) {
    if (needle.empty()) return true;
    return std::ranges::any_of(item.haystacks, [&](const std::string &haystack) {
        return match == CompactPickerMatch::PREFIX ? haystack.starts_with(needle) : haystack.contains(needle);
    });
}

struct QueryWindow {
    std::string_view text;
    i32 cursor_column = 0;
};

QueryWindow query_window(std::string_view query, usize cursor, i32 available_cells) {
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

} // namespace

void CompactPicker::set_items(std::vector<CompactPickerItem> next) {
    const auto kept = highlighted_id() ? std::optional<std::string>(*highlighted_id()) : std::nullopt;
    items = std::move(next);
    refilter(kept);
}

void CompactPicker::set_query(std::string_view next) {
    const auto kept = highlighted_id() ? std::optional<std::string>(*highlighted_id()) : std::nullopt;
    query = std::string(next);
    query_cursor = query.size();
    refilter(kept);
}

void CompactPicker::edit_query(PickerQueryEdit edit, std::string_view text) {
    query_cursor = std::min(query_cursor, query.size());
    switch (edit) {
        case PickerQueryEdit::LEFT: query_cursor = previous_grapheme_boundary(query, query_cursor); return;
        case PickerQueryEdit::RIGHT: query_cursor = next_grapheme_boundary(query, query_cursor); return;
        case PickerQueryEdit::WORD_LEFT: query_cursor = previous_word_boundary(query, query_cursor); return;
        case PickerQueryEdit::WORD_RIGHT: query_cursor = next_word_boundary(query, query_cursor); return;
        case PickerQueryEdit::HOME: query_cursor = 0; return;
        case PickerQueryEdit::END: query_cursor = query.size(); return;
        case PickerQueryEdit::INSERT: {
            query.insert(query_cursor, text.data(), text.size());
            query_cursor += text.size();
            break;
        }
        case PickerQueryEdit::BACKSPACE: {
            const auto boundary = previous_grapheme_boundary(query, query_cursor);
            if (boundary == query_cursor) return;
            query.erase(boundary, query_cursor - boundary);
            query_cursor = boundary;
            break;
        }
        case PickerQueryEdit::ERASE: {
            const auto boundary = next_grapheme_boundary(query, query_cursor);
            if (boundary == query_cursor) return;
            query.erase(query_cursor, boundary - query_cursor);
            break;
        }
        case PickerQueryEdit::BACKSPACE_WORD: {
            const auto boundary = previous_word_boundary(query, query_cursor);
            if (boundary == query_cursor) return;
            query.erase(boundary, query_cursor - boundary);
            query_cursor = boundary;
            break;
        }
        case PickerQueryEdit::ERASE_WORD: {
            const auto boundary = next_word_boundary(query, query_cursor);
            if (boundary == query_cursor) return;
            query.erase(query_cursor, boundary - query_cursor);
            break;
        }
    }
    const auto kept = highlighted_id() ? std::optional<std::string>(*highlighted_id()) : std::nullopt;
    refilter(kept);
}

void CompactPicker::refilter(const std::optional<std::string> &kept) {
    const auto needle = ascii_lower(query);
    filtered.clear();
    for (usize index = 0; index < items.size(); ++index) {
        if (matches(items[index], needle, match)) filtered.push_back(index);
    }
    highlight = 0;
    if (kept) {
        const auto restored = std::ranges::find_if(filtered, [&](usize index) { return items[index].id == *kept; });
        if (restored != filtered.end()) highlight = static_cast<usize>(restored - filtered.begin());
    }
}

void CompactPicker::move(i32 delta) noexcept {
    if (filtered.empty()) return;
    const auto position = static_cast<i64>(highlight) + delta;
    highlight = static_cast<usize>(std::clamp<i64>(position, 0, static_cast<i64>(filtered.size()) - 1));
}

std::optional<std::string_view> CompactPicker::highlighted_id() const noexcept {
    const auto *item = highlighted_item();
    if (!item) return std::nullopt;
    return item->id;
}

const CompactPickerItem *CompactPicker::highlighted_item() const noexcept {
    if (highlight >= filtered.size()) return nullptr;
    return &items[filtered[highlight]];
}

i32 CompactPicker::desired_rows(bool with_query_row) const noexcept {
    const auto results = loading || filtered.empty() ? usize{1} : std::min(filtered.size(), k_max_result_rows);
    return static_cast<i32>(results) + (error ? 1 : 0) + (with_query_row ? 1 : 0);
}

std::optional<Cursor> CompactPicker::project(Surface &surface, i32 first_row, i32 rows, bool with_query_row) const {
    lighter::check(first_row >= 0 && first_row + rows <= surface.rows, "compact picker band must fit its surface");
    if (rows <= 0) return std::nullopt;

    std::optional<Cursor> cursor;
    auto remaining = rows;
    if (with_query_row) {
        const auto row = first_row + rows - 1;
        const auto label_column = surface.write(row, 0, query_label.empty() ? "Search: " : query_label + ": ", Style::MUTED);
        const auto cursor_offset = std::min(query_cursor, query.size());
        const auto window = query_window(query, cursor_offset, surface.columns - label_column);
        surface.write(row, label_column, window.text, Style::NORMAL);
        const auto cursor_column = label_column + window.cursor_column;
        cursor = Cursor{.row = row, .column = std::clamp(cursor_column, 0, surface.columns - 1), .visible = true};
        --remaining;
    }
    if (error && remaining > 0) {
        surface.write(first_row + remaining - 1, 0, *error, Style::FAILURE);
        --remaining;
    }
    if (remaining <= 0) return cursor;

    if (loading) {
        surface.write(first_row + remaining - 1, 0, "Loading…", Style::MUTED);
        return cursor;
    }
    if (filtered.empty()) {
        surface.write(first_row + remaining - 1, 0, empty_message, Style::MUTED);
        return cursor;
    }

    const auto visible_count = std::min(static_cast<usize>(remaining), filtered.size());
    const auto start = highlight < visible_count ? usize{0} : highlight - visible_count + 1;
    for (usize offset = 0; offset < visible_count; ++offset) {
        const auto index = start + offset;
        const auto row = first_row + static_cast<i32>(offset);
        const bool selected = index == highlight;
        const auto &item = items[filtered[index]];
        auto column = surface.write(row, 0, selected ? "› " : "  ", selected ? Style::ACCENT : Style::MUTED);
        column = surface.write(row, column, item.primary, selected ? Style::EMPHASIS : Style::NORMAL);
        if (!item.annotation.empty()) {
            column = surface.write(row, column, " ", Style::MUTED);
            column = surface.write(row, column, item.annotation, Style::MUTED);
        }
        if (!item.description.empty()) {
            column = surface.write(row, column, " · ", Style::MUTED);
            column = surface.write(row, column, item.description, Style::MUTED);
        }
        if (item.current) {
            column = surface.write(row, column, " · ", Style::MUTED);
            surface.write(row, column, "current", Style::ACCENT);
        }
    }
    return cursor;
}

} // namespace liminal::tui
