#pragma once

#include <optional>
#include <string>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/error.h>
#include <liminal/tui/console_renderer.h>

namespace liminal::tui {

/// Interactive controller for the owned-query compact picker. Confirmation
/// records the highlighted identity; cancellation records an empty decision.
/// Enter with no matching result keeps the picker open.
struct CompactPickerDialog {
    lighter::Error begin(ConsoleRenderer &renderer, CompactPicker picker);
    bool active() const noexcept;
    lighter::Error set_items(std::vector<CompactPickerItem> items);
    lighter::Error fail(std::string detail);
    lighter::Error confirm();
    lighter::Error cancel();
    lighter::Task<std::optional<std::string>> next();

private:
    lighter::Error finish(std::optional<std::string> result);

    ConsoleRenderer *renderer = nullptr;
    std::optional<std::optional<std::string>> decision;
    lighter::Event ready;
};

} // namespace liminal::tui
