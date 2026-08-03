#include "repl.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/io/stream.h>
#include <lighter/async/io/terminal.h>
#include <lighter/async/runtime/interrupt.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/cancellation.h>

#include "liminal/tui/console_renderer.h"
#include "liminal/tui/transcript.h"

namespace liminal::tui {

using lighter::CancellationSource;
using lighter::fail;
using lighter::InterruptSource;
using lighter::Pipe;
using lighter::Stream;
using lighter::Task;
using lighter::TerminalEventKind;
using lighter::TerminalKey;
using lighter::TerminalSession;
using lighter::WhenAny;

namespace {

/// Splits redirected stdin on LF and owns the temporary one-line interactive
/// composer. Multiline paste remains a single prompt until Enter submits it.
struct PromptReader {
    Stream *input = nullptr;
    TerminalSession *terminal = nullptr;
    ConsoleRenderer *renderer = nullptr;
    std::string buffered;
    bool eof = false;

    Task<std::optional<std::string>, lighter::Error> next() {
        if (terminal) {
            co_return co_await next_terminal_prompt();
        }

        while (true) {
            if (auto pos = buffered.find('\n'); pos != std::string::npos) {
                auto line = buffered.substr(0, pos);
                buffered.erase(0, pos + 1);
                co_return finish(std::move(line));
            }
            if (eof) {
                if (buffered.empty()) {
                    co_return std::nullopt;
                }
                co_return finish(std::exchange(buffered, {}));
            }
            auto chunk = co_await input->read_chunk();
            if (!chunk) {
                if (chunk.error() == lighter::Error::k_end_of_file) {
                    eof = true;
                    continue;
                }
                co_await fail(std::move(chunk).error());
            }
            if (chunk->empty()) {
                eof = true;
                continue;
            }
            buffered.append(chunk->data(), chunk->size());
            input->consume(chunk->size());
        }
    }

private:
    Task<std::optional<std::string>, lighter::Error> next_terminal_prompt() {
        while (true) {
            auto event = co_await terminal->next_event().or_fail();
            switch (event.kind) {
                case TerminalEventKind::TEXT:
                case TerminalEventKind::PASTE:
                    buffered += event.text;
                    if (auto error = renderer->echo(event.text)) {
                        co_await fail(error);
                    }
                    break;
                case TerminalEventKind::KEY: {
                    if (!event.pressed) {
                        break;
                    }
                    if (event.key == TerminalKey::ENTER) {
                        if (auto error = renderer->write("\r\n")) {
                            co_await fail(error);
                        }
                        co_return finish(std::exchange(buffered, {}));
                    }
                    if (event.key == TerminalKey::BACKSPACE) {
                        if (!buffered.empty()) {
                            auto start = buffered.size() - 1;
                            while (start > 0 && (static_cast<unsigned char>(buffered[start]) & 0xc0) == 0x80) {
                                --start;
                            }
                            buffered.erase(start);
                            if (auto error = renderer->write("\b \b")) {
                                co_await fail(error);
                            }
                        }
                        break;
                    }
                    if (event.key == TerminalKey::TAB) {
                        buffered.push_back('\t');
                        if (auto error = renderer->write("\t")) {
                            co_await fail(error);
                        }
                        break;
                    }
                    const bool control = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::CONTROL);
                    const bool alt_gr = control && lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::ALT);
                    if (event.key == TerminalKey::CHARACTER && !event.text.empty() && (!control || alt_gr)) {
                        buffered += event.text;
                        if (auto error = renderer->echo(event.text)) {
                            co_await fail(error);
                        }
                    }
                    break;
                }
                case TerminalEventKind::CLOSED: co_return std::nullopt;
                case TerminalEventKind::RESIZE:
                case TerminalEventKind::FOCUS:
                case TerminalEventKind::MOUSE: break;
            }
        }
    }

    static std::string finish(std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }
};

struct TurnControl {
    CancellationSource *active_turn = nullptr;
};

struct UiSession {
    explicit UiSession(ConsoleRenderer &renderer) : renderer(&renderer) {}

    void apply(const Event &event) {
        transcript.apply(event);
        if (!render_error) {
            render_error = renderer->render(event);
        }
    }

    Transcript transcript;
    ConsoleRenderer *renderer;
    lighter::Error render_error;
};

constexpr std::string_view k_default_compact_instructions =
    "Compact the conversation while preserving the user's goals, constraints, decisions, "
    "important tool results, modified files, unresolved issues, and the context needed to continue.";

Task<void> signal_monitor(InterruptSource &interrupts, TurnControl &control) {
    while (true) {
        auto signal = co_await interrupts.next();
        if (!signal) {
            co_return;
        }
        if (interrupts.interrupt_count() >= 2) {
            std::fputs("\n[force exit]\n", stderr);
            std::exit(130);
        }
        if (control.active_turn) {
            control.active_turn->cancel();
            continue;
        }
        co_return;
    }
}

template <typename T, typename E>
Task<lighter::Outcome<T, E, lighter::Cancellation>> guard_turn(Task<T, E> work, TurnControl &control) {
    CancellationSource source;
    control.active_turn = &source;
    auto outcome = co_await with_token(std::move(work), source.token());
    control.active_turn = nullptr;
    co_return outcome;
}

Task<i32> repl_body(Agent &agent, PromptReader &reader, ConsoleRenderer &renderer, TurnControl &control, const ProviderFactory &factory) {
    UiSession ui(renderer);
    EventSink events = [&ui](const Event &event) { ui.apply(event); };

    while (true) {
        if (auto error = renderer.prompt(agent.provider.name, agent.provider.model)) {
            std::fprintf(stderr, "cannot render prompt: %s\n", std::string(error.message()).c_str());
            co_return 1;
        }

        auto line = co_await reader.next();
        if (!line) {
            std::fprintf(stderr, "cannot read prompt: %s\n", std::string(line.error().message()).c_str());
            co_return 1;
        }
        if (!line->has_value()) {
            co_return 0;
        }
        auto prompt = *std::move(*line);
        if (prompt.empty()) {
            continue;
        }
        if (prompt == "/quit" || prompt == "/exit") {
            co_return 0;
        }
        if (prompt == "/compact" || prompt.starts_with("/compact ")) {
            auto instructions =
                prompt == "/compact" ? std::string(k_default_compact_instructions) : prompt.substr(std::string_view("/compact ").size());
            auto outcome = co_await guard_turn(agent.compact(instructions), control);
            lighter::Error render_error;
            if (outcome.is_cancelled()) {
                render_error = renderer.notice("[compact cancelled; history unchanged]\n");
            } else if (outcome.has_error()) {
                render_error = renderer.notice("[compact error: " + outcome.error().message() + "]\n");
            } else {
                render_error = renderer.notice("[history compacted]\n");
            }
            if (render_error) {
                co_return 1;
            }
            continue;
        }
        if (prompt.starts_with("/switch")) {
            auto name = prompt == "/switch" ? std::string_view{} : std::string_view(prompt).substr(std::string_view("/switch ").size());
            if (name.empty()) {
                if (auto error = renderer.notice("usage: /switch <anthropic|openai>\n")) {
                    co_return 1;
                }
                continue;
            }
            auto next = factory(name);
            if (!next) {
                if (auto error = renderer.notice("[switch error: " + next.error().message() + "]\n")) {
                    co_return 1;
                }
                continue;
            }
            agent.switch_provider(*std::move(next));
            if (auto error = renderer.notice("[switched to " + agent.provider.name + ":" + agent.provider.model +
                                             "; private provider state from other providers is dropped]\n")) {
                co_return 1;
            }
            continue;
        }

        ui.apply(PromptSubmitted{.text = prompt});
        auto outcome = co_await guard_turn(agent.run_turn(std::move(prompt), events), control);
        if (outcome.is_cancelled()) {
            ui.apply(TurnCancelled{});
        } else if (outcome.has_error()) {
            ui.apply(TurnFailed{.message = outcome.error().message()});
        }
        if (ui.render_error) {
            std::fprintf(stderr, "cannot render session: %s\n", std::string(ui.render_error.message()).c_str());
            co_return 1;
        }
    }
}

} // namespace

Task<i32> run_repl(Agent &agent, InterruptSource &interrupts, ProviderFactory factory) {
    TerminalSession terminal;
    Pipe pipe;
    if (TerminalSession::attached(0)) {
        auto opened = TerminalSession::open();
        if (!opened) {
            std::fprintf(stderr, "cannot open terminal: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
        terminal = *std::move(opened);
    } else {
        auto opened = Pipe::open(0);
        if (!opened) {
            std::fprintf(stderr, "cannot open stdin pipe: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
        pipe = *std::move(opened);
    }

    const bool interactive = terminal.active();
    ConsoleRenderer renderer(interactive ? &terminal : nullptr);
    if (auto error = renderer.banner(agent.provider.name, agent.provider.model)) {
        std::fprintf(stderr, "cannot render banner: %s\n", std::string(error.message()).c_str());
        co_return 1;
    }

    PromptReader reader{
        .input = interactive ? nullptr : &pipe,
        .terminal = interactive ? &terminal : nullptr,
        .renderer = &renderer,
    };
    TurnControl control;
    auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, factory), signal_monitor(interrupts, control));
    if (raced.index() == 0) {
        co_return std::get<0>(raced);
    }
    co_return 130;
}

} // namespace liminal::tui
