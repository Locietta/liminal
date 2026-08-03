#include "repl.h"

#include <cstdio>
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
using lighter::usize;
using lighter::WhenAny;

namespace {

/// Splits redirected stdin on LF and routes native input into the interactive
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
                    if (auto error = renderer->insert(event.text)) {
                        co_await fail(error);
                    }
                    break;
                case TerminalEventKind::KEY: {
                    if (!event.pressed) {
                        break;
                    }
                    if (event.key == TerminalKey::ENTER) {
                        auto prompt = renderer->take_prompt();
                        if (auto error = renderer->redraw()) {
                            co_await fail(error);
                        }
                        co_return finish(std::move(prompt));
                    }
                    if (event.key == TerminalKey::BACKSPACE) {
                        if (auto error = renderer->backspace()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::TAB) {
                        if (auto error = renderer->insert("\t")) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::DELETE_KEY) {
                        if (auto error = renderer->erase()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::ARROW_LEFT) {
                        if (auto error = renderer->move_left()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::ARROW_RIGHT) {
                        if (auto error = renderer->move_right()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::HOME) {
                        if (auto error = renderer->move_home()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::END) {
                        if (auto error = renderer->move_end()) co_await fail(error);
                        break;
                    }
                    if (event.key == TerminalKey::PAGE_UP || event.key == TerminalKey::PAGE_DOWN) {
                        if (auto error = renderer->page(event.key == TerminalKey::PAGE_UP ? -1 : 1)) co_await fail(error);
                        break;
                    }
                    const bool control = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::CONTROL);
                    const bool alt_gr = control && lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::ALT);
                    if (event.key == TerminalKey::CHARACTER && !event.text.empty() && (!control || alt_gr)) {
                        if (auto error = renderer->insert(event.text)) co_await fail(error);
                    }
                    break;
                }
                case TerminalEventKind::CLOSED: co_return std::nullopt;
                case TerminalEventKind::RESIZE:
                    if (auto error = renderer->resize(event.size)) co_await fail(error);
                    break;
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
            co_return;
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

Task<i32> repl_body(Agent &agent, PromptReader &reader, ConsoleRenderer &renderer, TurnControl &control, model::Catalog &models) {
    lighter::Error render_error;
    EventSink events = [&renderer, &render_error](const Event &event) {
        if (!render_error) render_error = renderer.render(event);
    };

    while (true) {
        if (auto error = renderer.prompt(agent.model.entry.id, agent.model.reasoning_effort)) {
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
        if (prompt == "/model" || prompt.starts_with("/model ")) {
            auto refreshed = co_await guard_turn(models.refresh(), control);
            if (refreshed.is_cancelled()) {
                if (auto error = renderer.notice("[model refresh cancelled; selection unchanged]\n")) {
                    co_return 1;
                }
                continue;
            }
            if (refreshed.has_error()) {
                if (auto error = renderer.notice("[model error: " + refreshed.error().message() + "]\n")) {
                    co_return 1;
                }
                continue;
            }
            for (const auto &warning : refreshed->warnings) {
                if (auto error = renderer.notice("[model warning: " + warning + "]\n")) {
                    co_return 1;
                }
            }

            const auto selector =
                prompt == "/model" ? std::string_view{} : std::string_view(prompt).substr(std::string_view("/model ").size());
            if (selector.empty()) {
                std::string listing = "models (configure " + models.providers_file().string() + "):\n";
                for (const auto &entry : models.entries()) {
                    const bool selected = entry.provider == agent.model.entry.provider && entry.id == agent.model.entry.id;
                    listing += selected ? "* " : "  ";
                    listing += entry.provider + "/" + entry.id;
                    if (!entry.name.empty() && entry.name != entry.id) {
                        listing += " - " + entry.name;
                    }
                    if (!entry.reasoning_efforts.empty()) {
                        listing += " [effort: ";
                        for (usize index = 0; index < entry.reasoning_efforts.size(); ++index) {
                            if (index != 0) listing += ", ";
                            listing += entry.reasoning_efforts[index];
                        }
                        listing += "]";
                    }
                    listing += "\n";
                }
                listing += "select with /model <id>, <provider>/<id>, or <selector>@<effort>\n";
                if (auto error = renderer.notice(listing)) {
                    co_return 1;
                }
                continue;
            }

            auto next = models.select(selector);
            if (!next) {
                if (auto error = renderer.notice("[model error: " + next.error().message() + "]\n")) {
                    co_return 1;
                }
                continue;
            }
            agent.select_model(*std::move(next));
            auto notice = "[model: " + agent.model.entry.id;
            if (agent.model.reasoning_effort) {
                notice += "@" + *agent.model.reasoning_effort;
            }
            notice += "]\n";
            if (auto error = renderer.notice(notice)) {
                co_return 1;
            }
            continue;
        }

        render_error = renderer.render(PromptSubmitted{.text = prompt});
        auto outcome = co_await guard_turn(agent.run_turn(std::move(prompt), events), control);
        if (outcome.is_cancelled()) {
            if (!render_error) render_error = renderer.render(TurnCancelled{});
        } else if (outcome.has_error()) {
            if (!render_error) render_error = renderer.render(TurnFailed{.message = outcome.error().message()});
        }
        if (render_error) {
            std::fprintf(stderr, "cannot render session: %s\n", std::string(render_error.message()).c_str());
            co_return 1;
        }
    }
}

} // namespace

Task<i32> run_repl(Agent &agent, InterruptSource &interrupts, model::Catalog &models) {
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
    if (auto error = renderer.banner(agent.model.entry.id, agent.model.reasoning_effort)) {
        std::fprintf(stderr, "cannot render banner: %s\n", std::string(error.message()).c_str());
        co_return 1;
    }

    PromptReader reader{
        .input = interactive ? nullptr : &pipe,
        .terminal = interactive ? &terminal : nullptr,
        .renderer = &renderer,
    };
    TurnControl control;
    auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, models), signal_monitor(interrupts, control));
    if (raced.index() == 0) {
        co_return std::get<0>(raced);
    }
    co_return 130;
}

} // namespace liminal::tui
