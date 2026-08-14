#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

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
    LOAD_NEXT_PAGE,
    CONFIRMED,
    CANCELLED,
};

struct SelectableListPage {
    std::vector<SelectableListItem> items;
    bool has_more = false;
};

/// Reusable focused, paged list model. Selection is always represented by an
/// item identity; row positions are only a rendering concern.
struct SelectableList {
    SelectableList(std::string title, std::string empty_message, SelectableListPage first_page);

    SelectableListEffect apply(SelectableListAction action);
    void append_page(SelectableListPage page);
    void fail_page(std::string detail);
    std::optional<std::string_view> selected_id() const noexcept;
    Frame frame(i32 columns, i32 rows) const;

    std::string title;
    std::string description;
    std::string empty_message;
    std::vector<SelectableListPage> pages;
    usize page_index = 0;
    usize item_index = 0;
    bool waiting_for_page = false;
    std::optional<std::string> page_error;
};

} // namespace liminal::tui
