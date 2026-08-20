#include "selectable_list.h"

#include <algorithm>
#include <utility>

#include <lighter/utils/panic.h>

namespace liminal::tui {

namespace {

struct ItemLocation {
    usize page = 0;
    usize item = 0;
};

std::optional<ItemLocation> locate(const std::vector<SelectableListPage> &pages, const std::optional<std::string> &id) {
    if (!id) return std::nullopt;
    for (usize page = 0; page < pages.size(); ++page) {
        const auto found = std::ranges::find(pages[page].items, *id, &SelectableListItem::id);
        if (found != pages[page].items.end()) {
            return ItemLocation{.page = page, .item = static_cast<usize>(found - pages[page].items.begin())};
        }
    }
    return std::nullopt;
}

std::optional<std::string> first_identity(const std::vector<SelectableListPage> &pages) {
    for (const auto &page : pages) {
        if (!page.items.empty()) return page.items.front().id;
    }
    return std::nullopt;
}

std::optional<ItemLocation> selected_location(const std::vector<SelectableListPage> &pages, usize page, usize item,
                                              const std::optional<std::string> &id) {
    if (!id || page >= pages.size() || item >= pages[page].items.size() || pages[page].items[item].id != *id) return std::nullopt;
    return ItemLocation{.page = page, .item = item};
}

} // namespace

SelectableList::SelectableList(std::string title, std::string empty_message, SelectableListPage first_page)
    : title(std::move(title)), empty_message(std::move(empty_message)) {
    pages.push_back(std::move(first_page));
    selected_item_id = first_identity(pages);
}

void SelectableList::enable_query(std::string message, std::string label) {
    query_enabled = true;
    no_match_message = std::move(message);
    query_label = std::move(label);
}

void SelectableList::begin_query_load() {
    contract_assert(query_enabled && !waiting_for_page && !waiting_for_query);
    previous_query = query;
    previous_query_cursor = query_cursor;
    waiting_for_query = true;
    query_error.reset();
    page_error.reset();
}

SelectableListEffect SelectableList::apply(SelectableListAction action) {
    if ((waiting_for_page || waiting_for_query) && action != SelectableListAction::CANCEL) return SelectableListEffect::NONE;
    const auto location = selected_location(pages, page_index, item_index, selected_item_id);
    auto &page = pages[page_index];
    switch (action) {
        case SelectableListAction::UP:
            if (!location) return SelectableListEffect::NONE;
            if (location->item > 0) {
                --item_index;
                selected_item_id = page.items[item_index].id;
            } else if (page_index > 0) {
                --page_index;
                item_index = pages[page_index].items.empty() ? 0 : pages[page_index].items.size() - 1;
                if (!pages[page_index].items.empty()) selected_item_id = pages[page_index].items[item_index].id;
            } else if (page.has_previous) {
                waiting_for_page = true;
                pending_navigation = SelectableListPendingNavigation::UP;
                page_error.reset();
                return SelectableListEffect::LOAD_PREVIOUS_PAGE;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::DOWN:
            if (!location) return SelectableListEffect::NONE;
            if (location->item + 1 < page.items.size()) {
                ++item_index;
                selected_item_id = page.items[item_index].id;
            } else if (page_index + 1 < pages.size()) {
                ++page_index;
                item_index = 0;
                selected_item_id = pages[page_index].items.empty() ? std::nullopt : std::optional(pages[page_index].items.front().id);
            } else if (page.has_more) {
                waiting_for_page = true;
                pending_navigation = SelectableListPendingNavigation::DOWN;
                page_error.reset();
                return SelectableListEffect::LOAD_NEXT_PAGE;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::PAGE_UP:
            if (page_index > 0) {
                --page_index;
                if (!pages[page_index].items.empty()) {
                    item_index = location ? std::min(location->item, pages[page_index].items.size() - 1) : usize{0};
                    selected_item_id = pages[page_index].items[item_index].id;
                }
            } else if (page.has_previous) {
                waiting_for_page = true;
                pending_navigation = SelectableListPendingNavigation::PAGE_UP;
                page_error.reset();
                return SelectableListEffect::LOAD_PREVIOUS_PAGE;
            } else {
                item_index = 0;
                selected_item_id = page.items.empty() ? std::nullopt : std::optional(page.items.front().id);
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::PAGE_DOWN:
            if (page_index + 1 < pages.size()) {
                ++page_index;
                if (!pages[page_index].items.empty()) {
                    item_index = location ? std::min(location->item, pages[page_index].items.size() - 1) : usize{0};
                    selected_item_id = pages[page_index].items[item_index].id;
                }
            } else if (page.has_more) {
                waiting_for_page = true;
                pending_navigation = SelectableListPendingNavigation::PAGE_DOWN;
                page_error.reset();
                return SelectableListEffect::LOAD_NEXT_PAGE;
            } else if (!page.items.empty()) {
                item_index = page.items.size() - 1;
                selected_item_id = page.items.back().id;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::CONFIRM: return selected_id() ? SelectableListEffect::CONFIRMED : SelectableListEffect::NONE;
        case SelectableListAction::CANCEL: return SelectableListEffect::CANCELLED;
    }
    return SelectableListEffect::NONE;
}

SelectableListEffect SelectableList::edit_query(PickerQueryEdit edit, std::string_view text) {
    if (!query_enabled || waiting_for_page || waiting_for_query) return SelectableListEffect::NONE;
    const auto before = query;
    const auto before_cursor = query_cursor;
    if (!edit_picker_query(query, query_cursor, edit, text)) return SelectableListEffect::NONE;
    previous_query = before;
    previous_query_cursor = before_cursor;
    waiting_for_query = true;
    query_error.reset();
    page_error.reset();
    return SelectableListEffect::REPLACE_RESULTS;
}

void SelectableList::prepend_page(SelectableListPage page) {
    contract_assert(waiting_for_page);
    waiting_for_page = false;
    page_error.reset();
    const auto navigation = std::exchange(pending_navigation, SelectableListPendingNavigation::NONE);
    if (page.items.empty()) {
        pages.front().has_previous = false;
        return;
    }
    pages.front().has_previous = false;
    page.has_more = true;
    pages.insert(pages.begin(), std::move(page));
    ++page_index;
    if (navigation == SelectableListPendingNavigation::UP) {
        page_index = 0;
        item_index = pages.front().items.size() - 1;
        selected_item_id = pages.front().items[item_index].id;
    } else if (navigation == SelectableListPendingNavigation::PAGE_UP) {
        page_index = 0;
        item_index = std::min(item_index, pages.front().items.size() - 1);
        selected_item_id = pages.front().items[item_index].id;
    }
}

void SelectableList::append_page(SelectableListPage page) {
    contract_assert(waiting_for_page);
    waiting_for_page = false;
    page_error.reset();
    const auto navigation = std::exchange(pending_navigation, SelectableListPendingNavigation::NONE);
    if (page.items.empty()) {
        pages.back().has_more = false;
        return;
    }
    pages.back().has_more = false;
    page.has_previous = true;
    pages.push_back(std::move(page));
    if (navigation == SelectableListPendingNavigation::DOWN) {
        page_index = pages.size() - 1;
        item_index = 0;
        selected_item_id = pages.back().items.front().id;
    } else if (navigation == SelectableListPendingNavigation::PAGE_DOWN) {
        page_index = pages.size() - 1;
        item_index = std::min(item_index, pages.back().items.size() - 1);
        selected_item_id = pages.back().items[item_index].id;
    }
}

void SelectableList::replace_page(SelectableListPage page) {
    contract_assert(waiting_for_query);
    waiting_for_query = false;
    previous_query.reset();
    page_error.reset();
    query_error.reset();
    pages.clear();
    pages.push_back(std::move(page));
    page_index = 0;
    const auto kept = locate(pages, selected_item_id);
    item_index = kept ? kept->item : 0;
    if (!kept) selected_item_id = first_identity(pages);
}

void SelectableList::fail_page(std::string detail) {
    contract_assert(waiting_for_page);
    waiting_for_page = false;
    pending_navigation = SelectableListPendingNavigation::NONE;
    page_error = std::move(detail);
}

void SelectableList::fail_query(std::string detail) {
    contract_assert(waiting_for_query && previous_query);
    query = std::move(*previous_query);
    query_cursor = previous_query_cursor;
    previous_query.reset();
    waiting_for_query = false;
    query_error = std::move(detail);
}

std::optional<std::string_view> SelectableList::selected_id() const noexcept {
    if (!selected_location(pages, page_index, item_index, selected_item_id)) return std::nullopt;
    return selected_item_id;
}

Frame SelectableList::frame(i32 columns, i32 rows) const {
    contract_assert(columns > 0 && rows > 0);
    lighter::check(columns > 0 && rows > 0, "selectable list requires a positive surface size");
    Frame result{.surface = Surface(columns, rows)};
    result.surface.write(0, 0, title, Style::EMPHASIS);
    if (rows == 1) return result;
    i32 first_row = rows > 3 ? 2 : 1;
    if (!description.empty()) result.surface.write(1, 0, description, Style::MUTED);
    if (query_enabled && first_row < rows - 1) {
        const auto label_column = result.surface.write(first_row, 0, query_label + ": ", Style::MUTED);
        const auto window = picker_query_window(query, query_cursor, columns - label_column);
        result.surface.write(first_row, label_column, window.text, Style::NORMAL);
        result.cursor = {.row = first_row, .column = std::clamp(label_column + window.cursor_column, 0, columns - 1), .visible = true};
        ++first_row;
    }

    const auto location = selected_location(pages, page_index, item_index, selected_item_id);
    const auto current_page = std::min(page_index, pages.size() - 1);
    const auto &page = pages[current_page];
    const i32 footer_row = rows - 1;
    if ((waiting_for_page || waiting_for_query) && first_row < footer_row) {
        result.surface.write(first_row, 0, "Loading…", Style::MUTED);
        ++first_row;
    } else if (const auto &error = query_error ? query_error : page_error; error && first_row < footer_row) {
        result.surface.write(first_row, 0, *error, Style::FAILURE);
        ++first_row;
    }
    const auto visible_rows = std::max(footer_row - first_row, 0);
    if (page.items.empty()) {
        if (first_row < footer_row) {
            const auto &message = query_enabled && !query.empty() ? no_match_message : empty_message;
            result.surface.write(first_row, 0, message, Style::MUTED);
        }
    } else if (visible_rows > 0) {
        const auto visible_count = static_cast<usize>(visible_rows);
        const auto selected_item = location && location->page == current_page ? location->item : usize{0};
        const auto start = selected_item < visible_count ? usize{0} : selected_item - visible_count + 1;
        for (usize offset = 0; offset < visible_count && start + offset < page.items.size(); ++offset) {
            const auto index = start + offset;
            const auto row = first_row + static_cast<i32>(offset);
            const bool selected = location && location->page == current_page && index == location->item;
            auto column = result.surface.write(row, 0, selected ? "› " : "  ", selected ? Style::ACCENT : Style::MUTED);
            column = result.surface.write(row, column, page.items[index].primary, selected ? Style::EMPHASIS : Style::NORMAL);
            if (!page.items[index].secondary.empty()) {
                column = result.surface.write(row, column, " · ", Style::MUTED);
                result.surface.write(row, column, page.items[index].secondary, Style::MUTED);
            }
        }
    }
    if (footer_row > 0) {
        const auto page_label = "Page " + std::to_string(current_page + 1) + (page.has_more ? "+" : "");
        const auto controls = query_enabled ? "Type search · ↑↓ move · PgUp/PgDn page · Enter select · Esc cancel · " :
                                              "↑↓ move · PgUp/PgDn page · Enter select · Esc cancel · ";
        result.surface.write(footer_row, 0, controls + page_label, Style::MUTED);
    }
    return result;
}

} // namespace liminal::tui
