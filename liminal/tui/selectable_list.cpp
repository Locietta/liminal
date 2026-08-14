#include "selectable_list.h"

#include <algorithm>
#include <utility>

#include <lighter/utils/panic.h>

namespace liminal::tui {

SelectableList::SelectableList(std::string title, std::string empty_message, SelectableListPage first_page)
    : title(std::move(title)), empty_message(std::move(empty_message)) {
    pages.push_back(std::move(first_page));
}

SelectableListEffect SelectableList::apply(SelectableListAction action) {
    if (waiting_for_page && action != SelectableListAction::CANCEL) return SelectableListEffect::NONE;
    auto &page = pages[page_index];
    switch (action) {
        case SelectableListAction::UP:
            if (item_index > 0) {
                --item_index;
            } else if (page_index > 0) {
                --page_index;
                item_index = pages[page_index].items.empty() ? 0 : pages[page_index].items.size() - 1;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::DOWN:
            if (item_index + 1 < page.items.size()) {
                ++item_index;
            } else if (page_index + 1 < pages.size()) {
                ++page_index;
                item_index = 0;
            } else if (page.has_more) {
                waiting_for_page = true;
                page_error.reset();
                return SelectableListEffect::LOAD_NEXT_PAGE;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::PAGE_UP:
            if (page_index > 0) {
                --page_index;
                item_index = std::min(item_index, pages[page_index].items.empty() ? usize{0} : pages[page_index].items.size() - 1);
            } else {
                item_index = 0;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::PAGE_DOWN:
            if (page_index + 1 < pages.size()) {
                ++page_index;
                item_index = std::min(item_index, pages[page_index].items.empty() ? usize{0} : pages[page_index].items.size() - 1);
            } else if (page.has_more) {
                waiting_for_page = true;
                page_error.reset();
                return SelectableListEffect::LOAD_NEXT_PAGE;
            } else if (!page.items.empty()) {
                item_index = page.items.size() - 1;
            }
            return SelectableListEffect::NONE;
        case SelectableListAction::CONFIRM: return selected_id() ? SelectableListEffect::CONFIRMED : SelectableListEffect::NONE;
        case SelectableListAction::CANCEL: return SelectableListEffect::CANCELLED;
    }
    return SelectableListEffect::NONE;
}

void SelectableList::append_page(SelectableListPage page) {
    contract_assert(waiting_for_page);
    waiting_for_page = false;
    page_error.reset();
    if (page.items.empty()) {
        pages.back().has_more = false;
        return;
    }
    pages.push_back(std::move(page));
    page_index = pages.size() - 1;
    item_index = 0;
}

void SelectableList::fail_page(std::string detail) {
    contract_assert(waiting_for_page);
    waiting_for_page = false;
    page_error = std::move(detail);
}

std::optional<std::string_view> SelectableList::selected_id() const noexcept {
    if (pages.empty() || page_index >= pages.size() || item_index >= pages[page_index].items.size()) return std::nullopt;
    return pages[page_index].items[item_index].id;
}

Frame SelectableList::frame(i32 columns, i32 rows) const {
    contract_assert(columns > 0 && rows > 0);
    lighter::check(columns > 0 && rows > 0, "selectable list requires a positive surface size");
    Frame result{.surface = Surface(columns, rows)};
    result.surface.write(0, 0, title, Style::EMPHASIS);
    if (rows == 1) return result;
    if (!description.empty()) result.surface.write(1, 0, description, Style::MUTED);

    const auto &page = pages[page_index];
    const i32 first_row = rows > 3 ? 2 : 1;
    const i32 footer_row = rows - 1;
    const auto visible_rows = std::max(footer_row - first_row, 0);
    if (waiting_for_page) {
        if (first_row < footer_row) result.surface.write(first_row, 0, "Loading…", Style::MUTED);
    } else if (page_error) {
        if (first_row < footer_row) result.surface.write(first_row, 0, *page_error, Style::FAILURE);
    } else if (page.items.empty()) {
        if (first_row < footer_row) result.surface.write(first_row, 0, empty_message, Style::MUTED);
    } else if (visible_rows > 0) {
        const auto visible_count = static_cast<usize>(visible_rows);
        const auto start = item_index < visible_count ? usize{0} : item_index - visible_count + 1;
        for (usize offset = 0; offset < visible_count && start + offset < page.items.size(); ++offset) {
            const auto index = start + offset;
            const auto row = first_row + static_cast<i32>(offset);
            const bool selected = index == item_index;
            auto column = result.surface.write(row, 0, selected ? "› " : "  ", selected ? Style::ACCENT : Style::MUTED);
            column = result.surface.write(row, column, page.items[index].primary, selected ? Style::EMPHASIS : Style::NORMAL);
            if (!page.items[index].secondary.empty()) {
                column = result.surface.write(row, column, " · ", Style::MUTED);
                result.surface.write(row, column, page.items[index].secondary, Style::MUTED);
            }
        }
    }
    if (footer_row > 0) {
        const auto page_label = "Page " + std::to_string(page_index + 1) + (page.has_more ? "+" : "");
        result.surface.write(footer_row, 0, "↑↓ move · PgUp/PgDn page · Enter select · Esc cancel · " + page_label, Style::MUTED);
    }
    return result;
}

} // namespace liminal::tui
