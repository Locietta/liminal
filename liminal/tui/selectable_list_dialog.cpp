#include "selectable_list_dialog.h"

#include <utility>

namespace liminal::tui {

lighter::Error SelectableListDialog::begin(ConsoleRenderer &renderer, SelectableList list, LoadPage load_page) {
    this->renderer = &renderer;
    this->load_page = std::move(load_page);
    decision.reset();
    ready.reset();
    return renderer.show_selectable_list(std::move(list));
}

bool SelectableListDialog::active() const noexcept { return renderer && renderer->selectable_list_active(); }

lighter::Error SelectableListDialog::apply(SelectableListAction action) {
    SelectableListEffect effect = SelectableListEffect::NONE;
    if (auto error = renderer->apply_selectable_list(action, effect)) return error;
    if (effect == SelectableListEffect::LOAD_NEXT_PAGE) {
        auto page = load_page();
        return page ? renderer->append_selectable_list_page(*std::move(page)) :
                      renderer->fail_selectable_list_page("Cannot load sessions: " + page.error().detail);
    }
    if (effect == SelectableListEffect::CONFIRMED) {
        decision = std::string(*renderer->selectable_list_selection());
    } else if (effect == SelectableListEffect::CANCELLED) {
        decision = std::optional<std::string>{};
    } else {
        return {};
    }
    if (auto error = renderer->close_selectable_list()) return error;
    renderer = nullptr;
    ready.set();
    return {};
}

lighter::Task<std::optional<std::string>> SelectableListDialog::next() {
    while (!decision.has_value()) co_await ready.wait();
    auto result = std::move(*decision);
    decision.reset();
    ready.reset();
    co_return result;
}

} // namespace liminal::tui
