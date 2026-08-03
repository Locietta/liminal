#pragma once

#include <string_view>

#include <lighter/async/io/terminal.h>
#include <lighter/async/vocab/error.h>

#include "liminal/event.h"

namespace liminal::tui {

/// Transitional plain renderer. It is the sole writer for both interactive
/// terminal bytes and redirected stdout while the cell renderer is built.
struct ConsoleRenderer {
    explicit ConsoleRenderer(lighter::TerminalSession *terminal = nullptr) : terminal(terminal) {}

    lighter::Error write(std::string_view text);
    lighter::Error echo(std::string_view text);
    lighter::Error render(const Event &event);
    lighter::Error banner(std::string_view provider, std::string_view model);
    lighter::Error prompt(std::string_view provider, std::string_view model);
    lighter::Error notice(std::string_view text);

    lighter::TerminalSession *terminal;
};

} // namespace liminal::tui
