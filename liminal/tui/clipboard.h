#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

#include <liminal/error.h>

namespace liminal::session {
struct Session;
} // namespace liminal::session

namespace liminal::tui {

/// Returns no value when the prompt is not a /copy command. Valid commands
/// contain the newest-first reply ordinal, while malformed commands fail.
Result<std::optional<lighter::types::usize>> parse_copy_command(std::string_view prompt);

/// Copies UTF-8 text to the host clipboard. Windows uses the native Unicode
/// clipboard; Linux delegates to the active Wayland, X11, or WSL helper.
lighter::Task<void, Error> copy_to_clipboard(std::string text);

/// Copies the Nth-newest textual reply from the active session branch.
lighter::Task<void, Error> copy_session_reply(const session::Session &session, lighter::types::usize ordinal = 1);

} // namespace liminal::tui
