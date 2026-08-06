#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <lighter/async/io/terminal.h>
#include <lighter/async/vocab/error.h>

#include <liminal/event.h>
#include <liminal/tui/session_screen.h>

namespace liminal::tui {

/// Sole output boundary. Interactive sessions render application-owned full
/// frames; redirected sessions retain append-only plain output.
struct ConsoleRenderer {
    explicit ConsoleRenderer(lighter::TerminalSession *terminal = nullptr, bool mirror_plain_output = false)
        : terminal(terminal), mirror_plain_output(mirror_plain_output) {}

    lighter::Error write(std::string_view text);
    lighter::Error render(const Event &event);
    lighter::Error banner(std::string_view model, const std::optional<std::string> &effort);
    lighter::Error prompt(std::string_view model, const std::optional<std::string> &effort);
    lighter::Error notice(std::string_view text);

    lighter::Error insert(std::string_view text);
    lighter::Error backspace();
    lighter::Error erase();
    lighter::Error backspace_word();
    lighter::Error erase_word();
    lighter::Error move_left();
    lighter::Error move_right();
    lighter::Error move_word_left();
    lighter::Error move_word_right();
    lighter::Error move_up();
    lighter::Error move_down();
    lighter::Error move_home();
    lighter::Error move_end();
    lighter::Error move_document_home();
    lighter::Error move_document_end();
    lighter::Error page(i32 direction);
    lighter::Error resize(lighter::TerminalSize size);
    lighter::Error redraw();
    lighter::Error flush();
    void set_redraw_scheduler(std::copyable_function<void()> scheduler);
    lighter::Error clear_prompt();
    bool prompt_empty() const noexcept;
    std::string take_prompt();

    lighter::TerminalSession *terminal;
    bool mirror_plain_output = false;
    SessionScreen screen;
    std::optional<Frame> previous_frame;
    std::copyable_function<void()> redraw_scheduler;
    bool redraw_pending = false;
};

} // namespace liminal::tui
