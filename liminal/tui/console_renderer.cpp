#include "console_renderer.h"

#include <concepts>
#include <cstdio>
#include <string>
#include <type_traits>

namespace liminal::tui {

lighter::Error ConsoleRenderer::write(std::string_view text) {
    if (terminal) {
        return terminal->write(text);
    }
    while (!text.empty()) {
        const auto written = std::fwrite(text.data(), 1, text.size(), stdout);
        if (written == 0) {
            return lighter::Error::k_io_error;
        }
        text.remove_prefix(written);
    }
    return {};
}

lighter::Error ConsoleRenderer::render(const Event &event) {
    if (terminal) {
        screen.apply(event);
        return redraw();
    }
    return std::visit(
        [this](const auto &value) -> lighter::Error {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<T, AssistantTextDelta>) {
                return write(value.text);
            } else if constexpr (std::same_as<T, ToolStarted>) {
                return write("\n[running tool: " + value.name + "]\n");
            } else if constexpr (std::same_as<T, TurnCompleted>) {
                return write("\n");
            } else if constexpr (std::same_as<T, TurnCancelled>) {
                return write("\n[turn cancelled; discarded from history - command side effects may remain]\n");
            } else if constexpr (std::same_as<T, TurnFailed>) {
                return write("\n[error: " + value.message + "]\n");
            }
            return {};
        },
        event);
}

lighter::Error ConsoleRenderer::banner(std::string_view model, const std::optional<std::string> &effort) {
    if (terminal) {
        auto current_size = terminal->size();
        if (!current_size) return current_size.error();
        screen.resize(*current_size);
        screen.set_model(model, effort);
        return redraw();
    }
    auto selection = std::string(model);
    if (effort) {
        selection += "@" + *effort;
    }
    return write("liminal - model: " + selection + " (tools run unsandboxed with your privileges)\n");
}

lighter::Error ConsoleRenderer::prompt(std::string_view model, const std::optional<std::string> &effort) {
    if (terminal) {
        screen.set_model(model, effort);
        return redraw();
    }
    auto selection = std::string(model);
    if (effort) {
        selection += "@" + *effort;
    }
    return write("\n" + selection + " > ");
}

lighter::Error ConsoleRenderer::notice(std::string_view text) {
    if (!terminal) return write(text);
    screen.add_notice(std::string(text));
    return redraw();
}

lighter::Error ConsoleRenderer::insert(std::string_view text) {
    screen.composer.insert(text);
    return redraw();
}

lighter::Error ConsoleRenderer::backspace() {
    screen.composer.backspace();
    return redraw();
}

lighter::Error ConsoleRenderer::erase() {
    screen.composer.erase();
    return redraw();
}

lighter::Error ConsoleRenderer::move_left() {
    screen.composer.move_left();
    return redraw();
}

lighter::Error ConsoleRenderer::move_right() {
    screen.composer.move_right();
    return redraw();
}

lighter::Error ConsoleRenderer::move_home() {
    screen.composer.move_home();
    return redraw();
}

lighter::Error ConsoleRenderer::move_end() {
    screen.composer.move_end();
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

lighter::Error ConsoleRenderer::redraw() {
    if (!terminal) return {};
    return terminal->write(encode_frame(screen.frame()));
}

std::string ConsoleRenderer::take_prompt() { return screen.composer.take(); }

} // namespace liminal::tui
