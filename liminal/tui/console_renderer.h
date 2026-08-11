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
    lighter::Error banner(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer);
    lighter::Error prompt(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer);
    lighter::Error notice(std::string_view text);
    lighter::Error status(std::string_view text);

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
    lighter::Error previous_prompt();
    lighter::Error next_prompt();
    lighter::Error move_home();
    lighter::Error move_end();
    lighter::Error move_document_home();
    lighter::Error move_document_end();
    lighter::Error replace_prompt(std::string text);
    lighter::Error set_external_editor_active(bool active);
    lighter::Error scroll(i32 rows);
    lighter::Error page(i32 direction);
    lighter::Error begin_selection(i32 row, i32 column);
    lighter::Error extend_selection(i32 row, i32 column);
    bool has_selection() const noexcept;
    lighter::Error clear_selection();
    std::string take_selection();
    lighter::Error resize(lighter::TerminalSize size);
    lighter::Error refresh_animation();
    lighter::Error redraw();
    lighter::Error flush();
    void pause_rendering() noexcept;
    lighter::Error resume_rendering();
    void set_redraw_scheduler(std::copyable_function<void()> scheduler);
    lighter::Error clear_prompt();
    bool prompt_empty() const noexcept;
    std::string prompt_text() const;
    std::string take_prompt();

    lighter::TerminalSession *terminal;
    bool mirror_plain_output = false;
    SessionScreen screen;
    std::optional<Frame> previous_frame;
    std::copyable_function<void()> redraw_scheduler;
    bool redraw_pending = false;
    bool rendering_paused = false;
};

} // namespace liminal::tui
