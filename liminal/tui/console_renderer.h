#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <lighter/async/io/terminal.h>
#include <lighter/async/vocab/error.h>

#include "liminal/event.h"
#include "liminal/tui/session_screen.h"

namespace liminal::tui {

/// Sole output boundary. Interactive sessions render application-owned full
/// frames; redirected sessions retain append-only plain output.
struct ConsoleRenderer {
    explicit ConsoleRenderer(lighter::TerminalSession *terminal = nullptr) : terminal(terminal) {}

    lighter::Error write(std::string_view text);
    lighter::Error render(const Event &event);
    lighter::Error banner(std::string_view model, const std::optional<std::string> &effort);
    lighter::Error prompt(std::string_view model, const std::optional<std::string> &effort);
    lighter::Error notice(std::string_view text);

    lighter::Error insert(std::string_view text);
    lighter::Error backspace();
    lighter::Error erase();
    lighter::Error move_left();
    lighter::Error move_right();
    lighter::Error move_home();
    lighter::Error move_end();
    lighter::Error page(i32 direction);
    lighter::Error resize(lighter::TerminalSize size);
    lighter::Error redraw();
    std::string take_prompt();

    lighter::TerminalSession *terminal;
    SessionScreen screen;
};

} // namespace liminal::tui
