#include "repl.h"

#include <deque>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <csignal>
#endif

#include <lighter/async/io/stream.h>
#include <lighter/async/io/terminal.h>
#include <lighter/async/runtime/interrupt.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/cancellation.h>
#include <lighter/async/io/watcher.h>

#include <liminal/tui/console_renderer.h>

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

struct TurnControl {
    CancellationSource *active_turn = nullptr;
};

struct SessionFailure {
    std::string message;

    void record(std::string_view context, lighter::Error error, TurnControl &control) {
        if (message.empty()) message = std::string(context) + ": " + std::string(error.message());
        if (control.active_turn) control.active_turn->cancel();
    }
};

struct PromptQueue {
    void push(std::string prompt) {
        pending.push_back(std::move(prompt));
        ready.set();
    }

    Task<std::string> next() {
        while (pending.empty()) co_await ready.wait();
        auto prompt = std::move(pending.front());
        pending.pop_front();
        if (pending.empty()) ready.reset();
        co_return prompt;
    }

    std::deque<std::string> pending;
    lighter::Event ready;
};

std::string finish_prompt(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

/// Splits redirected stdin on LF. Interactive input is delivered continuously
/// by terminal_input_loop, including while a provider turn is active.
struct PromptReader {
    Stream *input = nullptr;
    PromptQueue *prompts = nullptr;
    std::string buffered;
    bool eof = false;

    Task<std::optional<std::string>, lighter::Error> next() {
        if (prompts) co_return co_await prompts->next();

        while (true) {
            if (auto pos = buffered.find('\n'); pos != std::string::npos) {
                auto line = buffered.substr(0, pos);
                buffered.erase(0, pos + 1);
                co_return finish_prompt(std::move(line));
            }
            if (eof) {
                if (buffered.empty()) {
                    co_return std::nullopt;
                }
                co_return finish_prompt(std::exchange(buffered, {}));
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
};

lighter::Error apply_terminal_event(const lighter::TerminalEvent &event, ConsoleRenderer &renderer, PromptQueue &prompts) {
    switch (event.kind) {
        case TerminalEventKind::TEXT:
        case TerminalEventKind::PASTE: return renderer.insert(event.text);
        case TerminalEventKind::KEY: {
            if (!event.pressed) return {};
            const bool control = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::CONTROL);
            const bool shift = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::SHIFT);
            const bool alt_gr = control && lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::ALT);
            if (event.key == TerminalKey::ENTER) {
                if (shift) return renderer.insert("\n");
                prompts.push(finish_prompt(renderer.take_prompt()));
                return renderer.redraw();
            }
            if (event.key == TerminalKey::BACKSPACE) return control ? renderer.backspace_word() : renderer.backspace();
            if (event.key == TerminalKey::TAB) return renderer.insert("\t");
            if (event.key == TerminalKey::DELETE_KEY) return control ? renderer.erase_word() : renderer.erase();
            if (event.key == TerminalKey::ARROW_LEFT) return control ? renderer.move_word_left() : renderer.move_left();
            if (event.key == TerminalKey::ARROW_RIGHT) return control ? renderer.move_word_right() : renderer.move_right();
            if (event.key == TerminalKey::ARROW_UP) return renderer.move_up();
            if (event.key == TerminalKey::ARROW_DOWN) return renderer.move_down();
            if (event.key == TerminalKey::HOME) return control ? renderer.move_document_home() : renderer.move_home();
            if (event.key == TerminalKey::END) return control ? renderer.move_document_end() : renderer.move_end();
            if (event.key == TerminalKey::PAGE_UP || event.key == TerminalKey::PAGE_DOWN) {
                return renderer.page(event.key == TerminalKey::PAGE_UP ? -1 : 1);
            }
            if (event.key == TerminalKey::CHARACTER && control && !alt_gr && event.text == "j") return renderer.insert("\n");
            if (event.key == TerminalKey::CHARACTER && !event.text.empty() && (!control || alt_gr)) {
                return renderer.insert(event.text);
            }
            return {};
        }
        case TerminalEventKind::RESIZE: return renderer.resize(event.size);
        case TerminalEventKind::CLOSED:
        case TerminalEventKind::FOCUS:
        case TerminalEventKind::MOUSE: return {};
    }
    return {};
}

Task<i32> terminal_input_loop(TerminalSession &terminal, ConsoleRenderer &renderer, PromptQueue &prompts, TurnControl &control,
                              SessionFailure &failure) {
    while (true) {
        auto event = co_await terminal.next_event();
        if (!event) {
            failure.record("cannot read terminal input", event.error(), control);
            co_return 1;
        }
        if (event->kind == TerminalEventKind::CLOSED) co_return 0;
        if (auto error = apply_terminal_event(*event, renderer, prompts)) {
            failure.record("cannot render terminal input", error, control);
            co_return 1;
        }
    }
}

Task<i32> render_monitor(lighter::Event &requested, ConsoleRenderer &renderer, TurnControl &control, SessionFailure &failure) {
    while (true) {
        co_await requested.wait();
        co_await lighter::yield();
        requested.reset();
        if (auto error = renderer.flush()) {
            failure.record("cannot render coalesced frame", error, control);
            co_return 1;
        }
    }
}

constexpr std::string_view k_default_compact_instructions =
    "Compact the conversation while preserving the user's goals, constraints, decisions, "
    "important tool results, modified files, unresolved issues, and the context needed to continue.";

Task<i32> signal_monitor(InterruptSource &interrupts, ConsoleRenderer &renderer, TurnControl &control, SessionFailure &failure) {
    while (true) {
        auto signal = co_await interrupts.next();
        if (!signal) {
            failure.record("cannot watch process controls", signal.error(), control);
            co_return 1;
        }

        if (*signal == lighter::ControlEventKind::INTERRUPT) {
            // Match Codex's bottom-pane-first routing: local draft state owns
            // the press before active work, and only an idle empty session exits.
            if (!renderer.prompt_empty()) {
                if (auto error = renderer.clear_prompt()) {
                    failure.record("cannot clear prompt", error, control);
                    co_return 1;
                }
                continue;
            }
            if (control.active_turn) {
                control.active_turn->cancel();
                continue;
            }
        }
        co_return 130;
    }
}

#ifndef _WIN32
Task<i32> suspend_monitor(lighter::ControlEventSource &controls, TerminalSession &terminal, ConsoleRenderer &renderer, TurnControl &control,
                          SessionFailure &failure) {
    while (true) {
        auto event = co_await controls.next();
        if (!event) {
            failure.record("cannot watch terminal suspension", event.error(), control);
            co_return 1;
        }
        if (auto error = terminal.suspend()) {
            failure.record("cannot suspend terminal", error, control);
            co_return 1;
        }
        if (std::raise(SIGSTOP) != 0) {
            failure.record("cannot suspend process", lighter::Error::k_io_error, control);
            co_return 1;
        }
        if (auto error = terminal.resume()) {
            failure.record("cannot resume terminal", error, control);
            co_return 1;
        }
        auto size = terminal.size();
        if (!size) {
            failure.record("cannot read terminal size after resume", size.error(), control);
            co_return 1;
        }
        if (auto error = renderer.resize(*size)) {
            failure.record("cannot redraw terminal after resume", error, control);
            co_return 1;
        }
    }
}
#endif

template <typename T, typename E>
Task<lighter::Outcome<T, E, lighter::Cancellation>> guard_turn(Task<T, E> work, TurnControl &control) {
    CancellationSource source;
    control.active_turn = &source;
    auto outcome = co_await with_token(std::move(work), source.token());
    control.active_turn = nullptr;
    co_return outcome;
}

Task<i32> repl_body(Agent &agent, PromptReader &reader, ConsoleRenderer &renderer, TurnControl &control, model::Catalog &models,
                    SessionFailure &failure) {
    lighter::Error render_error;
    EventSink events = [&renderer, &render_error, &control](const Event &event) {
        if (render_error) return;
        render_error = renderer.render(event);
        if (render_error && control.active_turn) control.active_turn->cancel();
    };
    auto rendered = [&failure, &control](lighter::Error error, std::string_view context) {
        if (!error) return true;
        failure.record(context, error, control);
        return false;
    };

    while (true) {
        if (!rendered(renderer.prompt(agent.model.entry.id, agent.model.reasoning_effort), "cannot render prompt")) co_return 1;

        auto line = co_await reader.next();
        if (!line) {
            failure.record("cannot read prompt", line.error(), control);
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
            if (!rendered(render_error, "cannot render compaction status")) co_return 1;
            continue;
        }
        if (prompt == "/model" || prompt.starts_with("/model ")) {
            auto refreshed = co_await guard_turn(models.refresh(), control);
            if (refreshed.is_cancelled()) {
                if (!rendered(renderer.notice("[model refresh cancelled; selection unchanged]\n"), "cannot render model status"))
                    co_return 1;
                continue;
            }
            if (refreshed.has_error()) {
                if (!rendered(renderer.notice("[model error: " + refreshed.error().message() + "]\n"), "cannot render model error"))
                    co_return 1;
                continue;
            }
            for (const auto &warning : refreshed->warnings) {
                if (!rendered(renderer.notice("[model warning: " + warning + "]\n"), "cannot render model warning")) co_return 1;
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
                if (!rendered(renderer.notice(listing), "cannot render model catalog")) co_return 1;
                continue;
            }

            auto next = models.select(selector);
            if (!next) {
                if (!rendered(renderer.notice("[model error: " + next.error().message() + "]\n"), "cannot render model error")) co_return 1;
                continue;
            }
            agent.select_model(*std::move(next));
            if (!rendered(renderer.render(ModelSelected{.name = agent.model.entry.id, .effort = agent.model.reasoning_effort}),
                          "cannot render model selection"))
                co_return 1;
            continue;
        }

        render_error = renderer.render(PromptSubmitted{.text = prompt});
        if (render_error) {
            failure.record("cannot render submitted prompt", render_error, control);
            co_return 1;
        }
        auto outcome = co_await guard_turn(agent.run_turn(std::move(prompt), events), control);
        if (render_error) {
            failure.record("cannot render session", render_error, control);
            co_return 1;
        }
        if (outcome.is_cancelled()) {
            if (!render_error) render_error = renderer.render(TurnCancelled{});
        } else if (outcome.has_error()) {
            if (!render_error) render_error = renderer.render(TurnFailed{.message = outcome.error().message()});
        }
        if (render_error) {
            failure.record("cannot render session", render_error, control);
            co_return 1;
        }
    }
}

} // namespace

Task<i32> run_repl(Agent &agent, InterruptSource &interrupts, model::Catalog &models) {
    TerminalSession terminal;
    Pipe pipe;
    const bool input_attached = TerminalSession::attached(0);
    const bool output_attached = TerminalSession::attached(1);
    const bool error_attached = TerminalSession::attached(2);
    bool interactive = false;
    bool mirror_plain_output = false;
    i32 terminal_output = output_attached ? 1 : 2;
    bool try_terminal = input_attached && (output_attached || error_attached);
#ifdef _WIN32
    // ConPTY exposes stdout through a pipe-shaped CRT handle while CONOUT$ is
    // still the correct interactive output. TerminalSession::open validates
    // that distinction; a native Console with redirected stdout fails it and
    // falls through to the plain stream path below.
    if (input_attached && !output_attached && !error_attached) terminal_output = 1;
    try_terminal = input_attached;
#endif
    if (try_terminal) {
        auto opened = TerminalSession::open(0, terminal_output);
        if (opened) {
            terminal = *std::move(opened);
            interactive = true;
            mirror_plain_output = terminal_output == 2;
        } else if (output_attached) {
            std::fprintf(stderr, "cannot open terminal: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
    }
    if (!interactive) {
        auto opened = Pipe::open(0);
        if (!opened) {
            std::fprintf(stderr, "cannot open stdin stream: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
        pipe = *std::move(opened);
    }

    ConsoleRenderer renderer(interactive ? &terminal : nullptr, mirror_plain_output);
    TurnControl control;
    SessionFailure failure;
    i32 exit_code = 0;
#ifndef _WIN32
    lighter::ControlEventSource suspend_controls;
    if (interactive) {
        auto controls = lighter::ControlEventSource::create({lighter::ControlEventKind::SUSPEND});
        if (!controls) {
            failure.record("cannot watch terminal suspension", controls.error(), control);
            exit_code = 1;
        } else {
            suspend_controls = *std::move(controls);
        }
    }
#endif

    if (exit_code == 0) {
        if (auto error = renderer.banner(agent.model.entry.id, agent.model.reasoning_effort)) {
            failure.record("cannot render banner", error, control);
            exit_code = 1;
        } else if (interactive) {
            PromptQueue prompts;
            PromptReader reader{.prompts = &prompts};
            lighter::Event render_requested;
            renderer.set_redraw_scheduler([&render_requested] { render_requested.set(); });
#ifndef _WIN32
            auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, models, failure),
                                          signal_monitor(interrupts, renderer, control, failure),
                                          terminal_input_loop(terminal, renderer, prompts, control, failure),
                                          render_monitor(render_requested, renderer, control, failure),
                                          suspend_monitor(suspend_controls, terminal, renderer, control, failure));
            if (raced.index() == 0) exit_code = std::get<0>(raced);
            if (raced.index() == 1) exit_code = std::get<1>(raced);
            if (raced.index() == 2) exit_code = std::get<2>(raced);
            if (raced.index() == 3) exit_code = std::get<3>(raced);
            if (raced.index() == 4) exit_code = std::get<4>(raced);
#else
            auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, models, failure),
                                          signal_monitor(interrupts, renderer, control, failure),
                                          terminal_input_loop(terminal, renderer, prompts, control, failure),
                                          render_monitor(render_requested, renderer, control, failure));
            if (raced.index() == 0) exit_code = std::get<0>(raced);
            if (raced.index() == 1) exit_code = std::get<1>(raced);
            if (raced.index() == 2) exit_code = std::get<2>(raced);
            if (raced.index() == 3) exit_code = std::get<3>(raced);
#endif
            renderer.set_redraw_scheduler({});
        } else {
            PromptReader reader{.input = &pipe};
            auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, models, failure),
                                          signal_monitor(interrupts, renderer, control, failure));
            exit_code = raced.index() == 0 ? std::get<0>(raced) : std::get<1>(raced);
        }
    }

    control.active_turn = nullptr;
    if (terminal.active()) {
        if (auto error = terminal.suspend(); error && failure.message.empty()) {
            failure.message = "cannot restore terminal: " + std::string(error.message());
            exit_code = 1;
        }
    }
    if (!failure.message.empty()) std::fprintf(stderr, "%s\n", failure.message.c_str());
    co_return exit_code;
}

} // namespace liminal::tui
