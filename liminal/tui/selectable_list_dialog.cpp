#include "selectable_list_dialog.h"

#include <utility>

namespace liminal::tui {

lighter::Error SelectableListDialog::begin(ConsoleRenderer &renderer, SelectableList list, LoadPage load_page) {
    this->renderer = &renderer;
    this->load_page = std::move(load_page);
    decision.reset();
    opened.reset();
    ready.reset();
    if (auto error = renderer.show_selectable_list(std::move(list))) return error;
    opened.set();
    return {};
}

bool SelectableListDialog::active() const noexcept { return renderer && renderer->selectable_list_active(); }

lighter::Task<> SelectableListDialog::wait_until_active() {
    if (!active()) co_await opened.wait();
}

lighter::Error SelectableListDialog::apply(SelectableListAction action) {
    SelectableListEffect effect = SelectableListEffect::NONE;
    if (auto error = renderer->apply_selectable_list(action, effect)) return error;
    if (effect == SelectableListEffect::LOAD_PREVIOUS_PAGE || effect == SelectableListEffect::LOAD_NEXT_PAGE) return load(effect);
    if (effect == SelectableListEffect::CONFIRMED) {
        decision = std::string(*renderer->selectable_list_selection());
    } else if (effect == SelectableListEffect::CANCELLED) {
        decision = std::optional<std::string>{};
    } else {
        return {};
    }
    if (auto error = renderer->close_selectable_list()) return error;
    renderer = nullptr;
    opened.reset();
    ready.set();
    return {};
}

lighter::Error SelectableListDialog::edit_query(PickerQueryEdit edit, std::string_view text) {
    SelectableListEffect effect = SelectableListEffect::NONE;
    if (auto error = renderer->edit_selectable_list_query(edit, text, effect)) return error;
    return effect == SelectableListEffect::REPLACE_RESULTS ? load(effect) : lighter::Error{};
}

lighter::Error SelectableListDialog::load(SelectableListEffect effect) {
    if (!load_page) return lighter::Error::k_invalid_argument;
    const auto load = effect == SelectableListEffect::REPLACE_RESULTS    ? SelectableListPageLoad::REPLACE :
                      effect == SelectableListEffect::LOAD_PREVIOUS_PAGE ? SelectableListPageLoad::PREVIOUS :
                                                                           SelectableListPageLoad::NEXT;
    const auto query = renderer->selectable_list_query();
    const auto preferred_id = load == SelectableListPageLoad::REPLACE ? renderer->selectable_list_selection() : std::nullopt;
    auto page = load_page(query, load, preferred_id);
    if (page) {
        if (load == SelectableListPageLoad::REPLACE) return renderer->replace_selectable_list_page(*std::move(page));
        if (load == SelectableListPageLoad::PREVIOUS) return renderer->prepend_selectable_list_page(*std::move(page));
        return renderer->append_selectable_list_page(*std::move(page));
    }
    if (load == SelectableListPageLoad::REPLACE) {
        return renderer->fail_selectable_list_query("Cannot search: " + page.error().detail);
    }
    const auto direction = load == SelectableListPageLoad::PREVIOUS ? "previous" : "next";
    return renderer->fail_selectable_list_page("Cannot load " + std::string(direction) + " page: " + page.error().detail);
}

lighter::Task<std::optional<std::string>> SelectableListDialog::next() {
    while (!decision.has_value()) co_await ready.wait();
    auto result = std::move(*decision);
    decision.reset();
    ready.reset();
    co_return result;
}

} // namespace liminal::tui
