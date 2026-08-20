#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

namespace liminal::tui {

using namespace lighter::types;

struct CompactPickerItem {
    std::string id;
    std::string primary;
    std::string annotation;
    std::string description;
    std::vector<std::string> haystacks;
    bool current = false;
};

enum struct CompactPickerMatch {
    PREFIX,
    SUBSTRING,
};

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

/// Bounded contextual selection state rendered as a band above the composer.
/// The picker owns query filtering and highlight identity; band placement,
/// focus routing, and confirmation semantics belong to its host surface.
/// Filtering preserves the highlighted item across query and item changes
/// whenever that item is still present in the filtered result.
struct CompactPicker {
    CompactPickerMatch match = CompactPickerMatch::SUBSTRING;
    std::string query_label;
    std::string empty_message = "No matches";
    std::string query;
    usize query_cursor = 0;
    std::vector<CompactPickerItem> items;
    std::vector<usize> filtered;
    usize highlight = 0;
    bool loading = false;
    std::optional<std::string> error;

    void set_items(std::vector<CompactPickerItem> next);
    void set_query(std::string_view next);
    /// Grapheme-aware cursor editing on the owned query; mutations refilter
    /// while preserving the highlighted identity.
    void edit_query(PickerQueryEdit edit, std::string_view text = {});
    void refilter(const std::optional<std::string> &kept);
    void move(i32 delta) noexcept;
    std::optional<std::string_view> highlighted_id() const noexcept;
    const CompactPickerItem *highlighted_item() const noexcept;

    /// Rows the band would use given unlimited height: bounded result rows,
    /// plus one row per active error and owned-query line.
    i32 desired_rows(bool with_query_row) const noexcept;

    /// Renders the band into rows [first_row, first_row + rows). The owned
    /// query line renders nearest the composer, preceded by any error row.
    /// Returns the query-row cursor when one is rendered.
    std::optional<Cursor> project(Surface &surface, i32 first_row, i32 rows, bool with_query_row) const;
};

} // namespace liminal::tui
