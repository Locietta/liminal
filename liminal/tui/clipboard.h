#pragma once

#include <string>

#include <lighter/async/runtime/task.h>

#include <liminal/error.h>

namespace liminal::tui {

/// Copies UTF-8 text to the host clipboard. Windows uses the native Unicode
/// clipboard; Linux delegates to the active Wayland, X11, or WSL helper.
lighter::Task<void, Error> copy_to_clipboard(std::string text);

} // namespace liminal::tui
