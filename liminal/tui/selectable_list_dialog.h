#pragma once

#include <functional>
#include <optional>
#include <string>

#include <lighter/async/async.h>

#include <liminal/error.h>
#include <liminal/tui/console_renderer.h>

namespace liminal::tui {

/// Interactive controller for a focused SelectableList. Page loading remains
/// injectable so catalogs can preserve their own cursor type and policy.
struct SelectableListDialog {
    using LoadPage = std::copyable_function<Result<SelectableListPage>()>;

    lighter::Error begin(ConsoleRenderer &renderer, SelectableList list, LoadPage load_page = {});
    bool active() const noexcept;
    lighter::Error apply(SelectableListAction action);
    lighter::Task<> wait_until_active();
    lighter::Task<std::optional<std::string>> next();

private:
    ConsoleRenderer *renderer = nullptr;
    LoadPage load_page;
    std::optional<std::optional<std::string>> decision;
    lighter::Event opened;
    lighter::Event ready;
};

} // namespace liminal::tui
