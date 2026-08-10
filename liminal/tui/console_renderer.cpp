#include "console_renderer.h"

#include <concepts>
#include <cstdio>
#include <string>
#include <type_traits>

namespace liminal::tui {

namespace {

std::string plain_text(std::string_view text) { return sanitize_terminal_text(text, true); }

lighter::Error write_stdout(std::string_view text) {
    while (!text.empty()) {
        const auto written = std::fwrite(text.data(), 1, text.size(), stdout);
        if (written == 0) return lighter::Error::k_io_error;
        text.remove_prefix(written);
    }
    return {};
}

lighter::Error render_plain(const Event &event) {
    return std::visit(
        [](const auto &value) -> lighter::Error {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<T, AssistantTextDelta>) {
                return write_stdout(plain_text(value.text));
            } else if constexpr (std::same_as<T, ToolStarted>) {
                if (!value.command.empty()) return write_stdout("\n[tool running] Running " + plain_text(value.command) + "\n");
                const auto description = value.description.empty() ? value.name : value.description;
                return write_stdout("\n[tool running] " + plain_text(description) + "\n");
            } else if constexpr (std::same_as<T, ToolCompleted>) {
                const auto description =
                    !value.command.empty() ? "Ran " + value.command : (value.description.empty() ? value.name : value.description);
                auto output = std::string("[tool ") + (value.is_error ? "failed] " : "completed] ") + plain_text(description);
                if (!value.summary.empty()) output += "\n  " + plain_text(value.summary);
                return write_stdout(output + "\n");
            } else if constexpr (std::same_as<T, TurnCompleted>) {
                return write_stdout("\n");
            } else if constexpr (std::same_as<T, TurnCancelled>) {
                return write_stdout("\n[turn cancelled; discarded from history - command side effects may remain]\n");
            } else if constexpr (std::same_as<T, TurnFailed>) {
                return write_stdout("\n[error: " + plain_text(value.message) + "]\n");
            } else if constexpr (std::same_as<T, SessionNotice>) {
                return write_stdout(plain_text(value.text));
            } else if constexpr (std::same_as<T, ModelSelected>) {
                auto selection = plain_text(value.name);
                if (value.effort) selection += "@" + plain_text(*value.effort);
                return write_stdout("[model: " + selection + "]\n");
            }
            return {};
        },
        event);
}

} // namespace

lighter::Error ConsoleRenderer::write(std::string_view text) {
    const auto safe = plain_text(text);
    if (terminal) {
        return terminal->write(safe);
    }
    return write_stdout(safe);
}

lighter::Error ConsoleRenderer::render(const Event &event) {
    if (terminal) {
        screen.apply(event);
        if (auto error = redraw()) return error;
        return mirror_plain_output ? render_plain(event) : lighter::Error{};
    }
    return render_plain(event);
}

lighter::Error ConsoleRenderer::banner(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer) {
    if (terminal) {
        auto current_size = terminal->size();
        if (!current_size) return current_size.error();
        screen.resize(*current_size);
        screen.set_model(model, effort);
        screen.set_footer(footer);
        if (auto error = redraw()) return error;
        if (!mirror_plain_output) return {};
    }
    auto selection = plain_text(model);
    if (effort) {
        selection += "@" + plain_text(*effort);
    }
    return write_stdout("liminal - model: " + selection + " (tools run unsandboxed with your privileges)\n");
}

lighter::Error ConsoleRenderer::prompt(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer) {
    if (terminal) {
        screen.set_model(model, effort);
        screen.set_footer(footer);
        if (auto error = redraw()) return error;
        if (!mirror_plain_output) return {};
    }
    auto selection = plain_text(model);
    if (effort) {
        selection += "@" + plain_text(*effort);
    }
    return write_stdout("\n" + selection + " > ");
}

lighter::Error ConsoleRenderer::notice(std::string_view text) { return render(SessionNotice{.text = std::string(text)}); }

lighter::Error ConsoleRenderer::insert(std::string_view text) {
    screen.insert(text);
    return redraw();
}

lighter::Error ConsoleRenderer::backspace() {
    screen.backspace();
    return redraw();
}

lighter::Error ConsoleRenderer::erase() {
    screen.erase();
    return redraw();
}

lighter::Error ConsoleRenderer::backspace_word() {
    screen.backspace_word();
    return redraw();
}

lighter::Error ConsoleRenderer::erase_word() {
    screen.erase_word();
    return redraw();
}

lighter::Error ConsoleRenderer::move_left() {
    screen.move_left();
    return redraw();
}

lighter::Error ConsoleRenderer::move_right() {
    screen.move_right();
    return redraw();
}

lighter::Error ConsoleRenderer::move_word_left() {
    screen.move_word_left();
    return redraw();
}

lighter::Error ConsoleRenderer::move_word_right() {
    screen.move_word_right();
    return redraw();
}

lighter::Error ConsoleRenderer::move_up() {
    screen.move_up();
    return redraw();
}

lighter::Error ConsoleRenderer::move_down() {
    screen.move_down();
    return redraw();
}

lighter::Error ConsoleRenderer::previous_prompt() {
    screen.previous_prompt();
    return redraw();
}

lighter::Error ConsoleRenderer::next_prompt() {
    screen.next_prompt();
    return redraw();
}

lighter::Error ConsoleRenderer::move_home() {
    screen.move_home();
    return redraw();
}

lighter::Error ConsoleRenderer::move_end() {
    screen.move_end();
    return redraw();
}

lighter::Error ConsoleRenderer::move_document_home() {
    screen.move_document_home();
    return redraw();
}

lighter::Error ConsoleRenderer::move_document_end() {
    screen.move_document_end();
    return redraw();
}

lighter::Error ConsoleRenderer::replace_prompt(std::string text) {
    screen.replace_prompt(std::move(text));
    return redraw();
}

lighter::Error ConsoleRenderer::set_external_editor_active(bool active) {
    screen.external_editor_active = active;
    return redraw();
}

lighter::Error ConsoleRenderer::scroll(i32 rows) {
    screen.scroll(rows);
    return redraw();
}

lighter::Error ConsoleRenderer::page(i32 direction) {
    screen.page(direction);
    return redraw();
}

lighter::Error ConsoleRenderer::resize(lighter::TerminalSize size) {
    screen.resize(size);
    return redraw();
}

lighter::Error ConsoleRenderer::refresh_elapsed_commands() {
    if (!screen.has_elapsed_running_command()) return {};
    return redraw();
}

lighter::Error ConsoleRenderer::redraw() {
    if (!terminal) return {};
    if (rendering_paused) {
        redraw_pending = true;
        return {};
    }
    if (redraw_scheduler) {
        if (!redraw_pending) {
            redraw_pending = true;
            redraw_scheduler();
        }
        return {};
    }
    return flush();
}

lighter::Error ConsoleRenderer::flush() {
    if (!terminal || rendering_paused) return {};
    redraw_pending = false;
    auto frame = screen.frame();
    auto encoded = encode_frame_diff(previous_frame ? &*previous_frame : nullptr, frame);
    if (!encoded.empty()) {
        if (auto error = terminal->write(encoded)) return error;
    }
    previous_frame = std::move(frame);
    return {};
}

void ConsoleRenderer::pause_rendering() noexcept { rendering_paused = true; }

lighter::Error ConsoleRenderer::resume_rendering() {
    rendering_paused = false;
    redraw_pending = false;
    previous_frame.reset();
    return redraw();
}

void ConsoleRenderer::set_redraw_scheduler(std::copyable_function<void()> scheduler) { redraw_scheduler = std::move(scheduler); }

lighter::Error ConsoleRenderer::clear_prompt() {
    screen.clear_prompt();
    return redraw();
}

bool ConsoleRenderer::prompt_empty() const noexcept { return screen.composer.empty(); }

std::string ConsoleRenderer::prompt_text() const { return screen.composer.text; }

std::string ConsoleRenderer::take_prompt() { return screen.take_prompt(); }

} // namespace liminal::tui
