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

lighter::Error ConsoleRenderer::echo(std::string_view text) {
    if (!terminal || !text.contains('\n')) {
        return write(text);
    }

    std::string output;
    output.reserve(text.size() + 8);
    char previous = 0;
    for (const char current : text) {
        if (current == '\n' && previous != '\r') {
            output.push_back('\r');
        }
        output.push_back(current);
        previous = current;
    }
    return write(output);
}

lighter::Error ConsoleRenderer::render(const Event &event) {
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

lighter::Error ConsoleRenderer::banner(std::string_view provider, std::string_view model) {
    return write("liminal - provider: " + std::string(provider) + ", model: " + std::string(model) +
                 " (tools run unsandboxed with your privileges)\n");
}

lighter::Error ConsoleRenderer::prompt(std::string_view provider, std::string_view model) {
    return write("\n" + std::string(provider) + ":" + std::string(model) + " > ");
}

lighter::Error ConsoleRenderer::notice(std::string_view text) { return write(text); }

} // namespace liminal::tui
