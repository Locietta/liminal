#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>
#include <liminal/tui/picker_query.h>

namespace liminal::tui {

using namespace lighter::types;

struct SelectableListItem {
    std::string id;
    std::string primary;
    std::string secondary;
};

enum struct SelectableListAction {
    UP,
    DOWN,
    PAGE_UP,
    PAGE_DOWN,
    CONFIRM,
    CANCEL,
};

enum struct SelectableListEffect {
    NONE,
    LOAD_PREVIOUS_PAGE,
    LOAD_NEXT_PAGE,
    REPLACE_RESULTS,
    CONFIRMED,
    CANCELLED,
};

struct SelectableListPage {
    std::vector<SelectableListItem> items;
    bool has_previous = false;
    bool has_more = false;
};

enum struct SelectableListPendingNavigation {
    NONE,
    UP,
    DOWN,
    PAGE_UP,
    PAGE_DOWN,
};

/// Reusable focused, paged list model. Selection is always represented by an
/// item identity; row positions are only a rendering concern.
struct SelectableList {
    SelectableList(std::string title, std::string empty_message, SelectableListPage first_page);

    void enable_query(std::string no_match_message, std::string label = "Search");
    void begin_query_load();
    SelectableListEffect apply(SelectableListAction action);
    SelectableListEffect edit_query(PickerQueryEdit edit, std::string_view text = {});
    void prepend_page(SelectableListPage page);
    void append_page(SelectableListPage page);
    void replace_page(SelectableListPage page);
    void fail_page(std::string detail);
    void fail_query(std::string detail);
    std::optional<std::string_view> selected_id() const noexcept;
    Frame frame(i32 columns, i32 rows) const;

    std::string title;
    std::string description;
    std::string empty_message;
    std::string no_match_message;
    std::string query_label = "Search";
    std::string query;
    usize query_cursor = 0;
    std::vector<SelectableListPage> pages;
    usize page_index = 0;
    usize item_index = 0;
    std::optional<std::string> selected_item_id;
    bool query_enabled = false;
    bool waiting_for_page = false;
    bool waiting_for_query = false;
    SelectableListPendingNavigation pending_navigation = SelectableListPendingNavigation::NONE;
    std::optional<std::string> page_error;
    std::optional<std::string> query_error;
    std::optional<std::string> previous_query;
    usize previous_query_cursor = 0;
};

} // namespace liminal::tui
