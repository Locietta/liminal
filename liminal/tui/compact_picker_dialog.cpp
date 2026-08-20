#include "compact_picker_dialog.h"

#include <utility>

namespace liminal::tui {

lighter::Error CompactPickerDialog::begin(ConsoleRenderer &renderer, CompactPicker picker) {
    this->renderer = &renderer;
    decision.reset();
    ready.reset();
    return renderer.open_picker(std::move(picker));
}

bool CompactPickerDialog::active() const noexcept { return renderer && renderer->model_picker_active(); }

lighter::Error CompactPickerDialog::set_items(std::vector<CompactPickerItem> items) {
    if (!active()) return lighter::Error::k_invalid_argument;
    return renderer->picker_set_items(std::move(items));
}

lighter::Error CompactPickerDialog::fail(std::string detail) {
    if (!active()) return lighter::Error::k_invalid_argument;
    return renderer->picker_fail(std::move(detail));
}

lighter::Error CompactPickerDialog::confirm() {
    if (!active()) return {};
    auto selection = renderer->picker_selection();
    if (!selection) return {};
    return finish(std::move(selection));
}

lighter::Error CompactPickerDialog::cancel() {
    if (!active()) return {};
    return finish(std::optional<std::string>{});
}

lighter::Error CompactPickerDialog::finish(std::optional<std::string> result) {
    decision = std::move(result);
    auto error = renderer->close_picker();
    renderer = nullptr;
    ready.set();
    return error;
}

lighter::Task<std::optional<std::string>> CompactPickerDialog::next() {
    while (!decision.has_value()) co_await ready.wait();
    auto result = std::move(*decision);
    decision.reset();
    ready.reset();
    co_return result;
}

} // namespace liminal::tui
