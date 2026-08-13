#include "repl.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <deque>
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
#include <liminal/tui/clipboard.h>
#include <liminal/tui/command.h>
#include <liminal/tui/external_editor.h>
#include <liminal/tui/hydration.h>
#include <liminal/session/persistence.h>

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

using namespace std::chrono_literals;

struct TaskControl {
    CancellationSource *active_task = nullptr;
};

struct SessionFailure {
    std::string message;

    void record(std::string_view context, lighter::Error error, TaskControl &control) {
        if (message.empty()) message = std::string(context) + ": " + std::string(error.message());
        if (control.active_task) control.active_task->cancel();
    }
};

SessionFooter session_footer(const Agent &agent) {
    SessionFooter footer{
        .workspace_path = agent.tools->working_directory.string(),
        .tokens_used = agent.session.tokens_used(),
        .not_saving = agent.session.persistence && agent.session.persistence->status().degraded,
    };
    const auto manifest = agent.context_manifest();
    if (!manifest || !manifest->usage.input_budget_tokens || !manifest->usage.remaining_input_tokens ||
        *manifest->usage.input_budget_tokens == 0) {
        return footer;
    }

    const auto budget = static_cast<i64>(*manifest->usage.input_budget_tokens);
    const auto remaining = std::clamp(*manifest->usage.remaining_input_tokens, i64{0}, budget);
    footer.context_left_percent = static_cast<u32>(remaining * 100 / budget);
    return footer;
}

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

struct ExternalEditorRequests {
    void request() {
        pending = true;
        ready.set();
    }

    Task<void> next() {
        while (!pending) co_await ready.wait();
        pending = false;
        ready.reset();
    }

    bool pending = false;
    lighter::Event ready;
};

struct CopyRequests {
    void request() {
        pending = true;
        ready.set();
    }

    Task<void> next() {
        while (!pending) co_await ready.wait();
        pending = false;
        ready.reset();
    }

    bool pending = false;
    lighter::Event ready;
};

struct SelectionCopies {
    void push(std::string text) {
        pending.push_back(std::move(text));
        ready.set();
    }

    Task<std::string> next() {
        while (pending.empty()) co_await ready.wait();
        auto text = std::move(pending.front());
        pending.pop_front();
        if (pending.empty()) ready.reset();
        co_return text;
    }

    std::deque<std::string> pending;
    lighter::Event ready;
};

std::string finish_prompt(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

/// Splits redirected stdin on LF. Interactive input is delivered continuously
/// by terminal_input_loop, including while a provider task is active.
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

lighter::Error apply_terminal_event(const lighter::TerminalEvent &event, ConsoleRenderer &renderer, PromptQueue &prompts,
                                    ExternalEditorRequests &editor_requests, CopyRequests &copy_requests, SelectionCopies &selection_copies,
                                    TaskControl &task_control, i32 &held_mouse_buttons) {
    switch (event.kind) {
        case TerminalEventKind::TEXT:
        case TerminalEventKind::PASTE: return renderer.insert(event.text);
        case TerminalEventKind::KEY: {
            if (!event.pressed) return {};
            const bool control = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::CONTROL);
            const bool shift = lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::SHIFT);
            const bool alt_gr = control && lighter::has_modifier(event.modifiers, lighter::TerminalModifiers::ALT);
            if (event.key == TerminalKey::ESCAPE) {
                if (task_control.active_task) task_control.active_task->cancel();
                return {};
            }
            if (event.key == TerminalKey::CHARACTER && control && !alt_gr && event.text == "g") {
                editor_requests.request();
                return {};
            }
            if (event.key == TerminalKey::CHARACTER && control && !alt_gr && event.text == "o") {
                copy_requests.request();
                return {};
            }
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
            if (event.key == TerminalKey::ARROW_UP) return control ? renderer.previous_prompt() : renderer.move_up();
            if (event.key == TerminalKey::ARROW_DOWN) return control ? renderer.next_prompt() : renderer.move_down();
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
        case TerminalEventKind::MOUSE: {
            if (event.wheel_delta != 0) {
                constexpr i32 rows_per_notch = 3;
                constexpr i32 wheel_delta_per_notch = 120;
                const auto notches = std::max(std::abs(event.wheel_delta) / wheel_delta_per_notch, 1);
                return renderer.scroll((event.wheel_delta > 0 ? -1 : 1) * rows_per_notch * notches);
            }
            const bool left = (event.mouse_buttons & 1) != 0;
            const bool was_left = (held_mouse_buttons & 1) != 0;
            held_mouse_buttons = event.mouse_buttons;
            if (left && !was_left) return renderer.begin_selection(event.y, event.x);
            if (left && was_left) return renderer.extend_selection(event.y, event.x);
            if (!left && was_left) {
                // The selection persists after release for an explicit copy
                // (Ctrl+C or Ctrl+O); a plain click just clears it.
                if (!renderer.has_selection()) return renderer.clear_selection();
                return {};
            }
            return {};
        }
        case TerminalEventKind::CLOSED:
        case TerminalEventKind::FOCUS: return {};
    }
    return {};
}

Task<i32> terminal_input_loop(TerminalSession &terminal, ConsoleRenderer &renderer, PromptQueue &prompts,
                              ExternalEditorRequests &editor_requests, CopyRequests &copy_requests, SelectionCopies &selection_copies,
                              TaskControl &control, SessionFailure &failure) {
    i32 held_mouse_buttons = 0;
    while (true) {
        auto event = co_await terminal.next_event();
        if (!event) {
            failure.record("cannot read terminal input", event.error(), control);
            co_return 1;
        }
        if (event->kind == TerminalEventKind::CLOSED) co_return 0;
        if (auto error = apply_terminal_event(*event, renderer, prompts, editor_requests, copy_requests, selection_copies, control,
                                              held_mouse_buttons)) {
            failure.record("cannot render terminal input", error, control);
            co_return 1;
        }
    }
}

Task<lighter::Error> copy_reply_with_feedback(Agent &agent, usize ordinal, ConsoleRenderer &renderer) {
    auto copied = co_await copy_session_reply(agent.session, ordinal);
    if (!copied) co_return renderer.notice("[copy error: " + copied.error().detail + "]\n");
    const auto status =
        ordinal == 1 ? std::string("Copied latest reply to clipboard") : "Copied reply " + std::to_string(ordinal) + " to clipboard";
    co_return renderer.status(status);
}

Task<i32> copy_reply_loop(CopyRequests &requests, SelectionCopies &selection_copies, Agent &agent, ConsoleRenderer &renderer,
                          TaskControl &control, SessionFailure &failure) {
    while (true) {
        co_await requests.next();
        if (renderer.has_selection()) {
            auto text = renderer.take_selection();
            if (!text.empty()) selection_copies.push(std::move(text));
            if (auto error = renderer.redraw()) {
                failure.record("cannot clear selection", error, control);
                co_return 1;
            }
            continue;
        }
        if (auto error = co_await copy_reply_with_feedback(agent, 1, renderer)) {
            failure.record("cannot render clipboard status", error, control);
            co_return 1;
        }
    }
}

Task<i32> selection_copy_loop(SelectionCopies &copies, ConsoleRenderer &renderer, TaskControl &control, SessionFailure &failure) {
    while (true) {
        auto text = co_await copies.next();
        auto copied = co_await copy_to_clipboard(std::move(text));
        const auto error =
            copied ? renderer.status("Copied selection to clipboard") : renderer.notice("[copy error: " + copied.error().detail + "]\n");
        if (error) {
            failure.record("cannot render clipboard status", error, control);
            co_return 1;
        }
    }
}

Task<i32> external_editor_loop(ExternalEditorRequests &requests, TerminalSession &terminal, ConsoleRenderer &renderer, TaskControl &control,
                               SessionFailure &failure) {
    while (true) {
        co_await requests.next();
        auto command = resolve_external_editor_command();
        if (!command) {
            if (auto error = renderer.notice("[editor error: " + command.error().detail + "]\n")) {
                failure.record("cannot render external editor error", error, control);
                co_return 1;
            }
            continue;
        }

        const auto seed = renderer.prompt_text();
        if (auto error = renderer.set_external_editor_active(true)) {
            failure.record("cannot render external editor status", error, control);
            co_return 1;
        }
        if (auto error = renderer.flush()) {
            failure.record("cannot flush external editor status", error, control);
            co_return 1;
        }
        renderer.pause_rendering();
        if (auto error = terminal.handoff()) {
            renderer.set_external_editor_active(false);
            renderer.resume_rendering();
            failure.record("cannot restore terminal for external editor", error, control);
            co_return 1;
        }

        auto edited = co_await run_external_editor(seed, *command);
        if (auto error = terminal.reclaim()) {
            failure.record("cannot resume terminal after external editor", error, control);
            co_return 1;
        }
        auto size = terminal.size();
        if (!size) {
            failure.record("cannot read terminal size after external editor", size.error(), control);
            co_return 1;
        }

        renderer.set_external_editor_active(false);
        if (!edited) {
            renderer.notice("[editor error: " + edited.error().detail + "]\n");
        } else {
            auto text = *std::move(edited);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
            renderer.replace_prompt(std::move(text));
        }
        renderer.resize(*size);
        if (auto error = renderer.resume_rendering()) {
            failure.record("cannot redraw after external editor", error, control);
            co_return 1;
        }
    }
}

Task<i32> render_monitor(lighter::Event &requested, ConsoleRenderer &renderer, TaskControl &control, SessionFailure &failure) {
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

Task<i32> animation_monitor(ConsoleRenderer &renderer, TaskControl &control, SessionFailure &failure) {
    auto timer = lighter::Timer::create();
    timer.start(100ms, 100ms);
    while (true) {
        auto tick = co_await timer.wait();
        if (!tick) {
            failure.record("cannot refresh task activity", tick.error(), control);
            co_return 1;
        }
        if (auto error = renderer.refresh_animation()) {
            failure.record("cannot render task activity", error, control);
            co_return 1;
        }
    }
}

constexpr std::string_view k_default_compact_instructions =
    "Compact the conversation while preserving the user's goals, constraints, decisions, "
    "important tool results, modified files, unresolved issues, and the context needed to continue.";

Task<i32> signal_monitor(InterruptSource &interrupts, ConsoleRenderer &renderer, TaskControl &control, SessionFailure &failure,
                         SelectionCopies *selection_copies = nullptr) {
    while (true) {
        auto signal = co_await interrupts.next();
        if (!signal) {
            failure.record("cannot watch process controls", signal.error(), control);
            co_return 1;
        }

        if (*signal == lighter::ControlEventKind::INTERRUPT) {
            // Windows-Terminal-like routing: an active selection turns the
            // press into a copy, then local draft state owns it before active
            // work, and only an idle empty session exits.
            if (selection_copies && renderer.has_selection()) {
                auto text = renderer.take_selection();
                if (!text.empty()) selection_copies->push(std::move(text));
                if (auto error = renderer.redraw()) {
                    failure.record("cannot clear selection", error, control);
                    co_return 1;
                }
                continue;
            }
            if (!renderer.prompt_empty()) {
                if (auto error = renderer.clear_prompt()) {
                    failure.record("cannot clear prompt", error, control);
                    co_return 1;
                }
                continue;
            }
            if (control.active_task) {
                control.active_task->cancel();
                continue;
            }
        }
        co_return 130;
    }
}

#ifndef _WIN32
Task<i32> suspend_monitor(lighter::ControlEventSource &controls, TerminalSession &terminal, ConsoleRenderer &renderer, TaskControl &control,
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
Task<lighter::Outcome<T, E, lighter::Cancellation>> guard_task(Task<T, E> work, TaskControl &control) {
    CancellationSource source;
    control.active_task = &source;
    auto outcome = co_await with_token(std::move(work), source.token());
    control.active_task = nullptr;
    co_return outcome;
}

Task<i32> repl_body(Agent &agent, PromptReader &reader, ConsoleRenderer &renderer, TaskControl &control, model::Catalog &models,
                    SessionFailure &failure) {
    lighter::Error render_error;
    EventSink events = [&renderer, &render_error, &control](const Event &event) {
        if (render_error) return;
        render_error = renderer.render(event);
        if (render_error && control.active_task) control.active_task->cancel();
    };
    auto rendered = [&failure, &control](lighter::Error error, std::string_view context) {
        if (!error) return true;
        failure.record(context, error, control);
        return false;
    };

    bool saving_notice_visible = false;
    while (true) {
        if (agent.session.persistence) {
            const auto persistence = agent.session.persistence->status();
            if (persistence.degraded && !saving_notice_visible) {
                if (!rendered(renderer.notice("[session not saving: " + persistence.detail + "]\n"), "cannot render persistence status"))
                    co_return 1;
                saving_notice_visible = true;
            } else if (!persistence.degraded) {
                saving_notice_visible = false;
            }
        }
        if (!rendered(renderer.prompt(agent.model.entry.id, agent.model.reasoning_effort, session_footer(agent)), "cannot render prompt"))
            co_return 1;

        auto line = co_await reader.next();
        if (!line) {
            failure.record("cannot read prompt", line.error(), control);
            co_return 1;
        }
        if (!line->has_value()) {
            co_return 0;
        }
        auto prompt = *std::move(*line);
        auto input = parse_repl_input(std::move(prompt));
        if (!input) {
            if (!rendered(renderer.notice("[command error: " + input.error().detail + "]\n"), "cannot render command error")) co_return 1;
            continue;
        }
        if (auto *user_prompt = std::get_if<UserPrompt>(&*input)) {
            prompt = std::move(user_prompt->text);
            if (prompt.empty()) continue;
        } else {
            auto command_line = std::move(*std::get_if<CommandLine>(&*input));
            auto command = resolve_command(command_line.name);
            if (!command) {
                if (!rendered(renderer.notice("[command error: " + command.error().detail + "]\n"), "cannot render command error"))
                    co_return 1;
                continue;
            }
            switch (*command) {
                case CommandKind::QUIT: {
                    auto arguments = require_no_arguments(command_line.name, command_line.arguments);
                    if (!arguments) {
                        if (!rendered(renderer.notice("[command error: " + arguments.error().detail + "]\n"),
                                      "cannot render command error"))
                            co_return 1;
                        continue;
                    }
                    co_return 0;
                }
                case CommandKind::COPY: {
                    auto arguments = parse_copy_arguments(command_line.arguments);
                    if (!arguments) {
                        if (!rendered(renderer.notice("[copy error: " + arguments.error().detail + "]\n"), "cannot render copy error"))
                            co_return 1;
                        continue;
                    }
                    if (!rendered(co_await copy_reply_with_feedback(agent, arguments->ordinal, renderer), "cannot render clipboard status"))
                        co_return 1;
                    continue;
                }
                case CommandKind::CONTEXT: {
                    auto arguments = require_no_arguments(command_line.name, command_line.arguments);
                    if (!arguments) {
                        if (!rendered(renderer.notice("[command error: " + arguments.error().detail + "]\n"),
                                      "cannot render command error"))
                            co_return 1;
                        continue;
                    }
                    auto manifest = agent.context_manifest();
                    if (!manifest) {
                        if (!rendered(renderer.notice("[context error: " + manifest.error().message() + "]\n"),
                                      "cannot render context error"))
                            co_return 1;
                    } else if (!rendered(renderer.notice(context::describe(*manifest)), "cannot render context manifest")) {
                        co_return 1;
                    }
                    continue;
                }
                case CommandKind::COMPACT: {
                    auto instructions =
                        command_line.arguments.empty() ? std::string(k_default_compact_instructions) : std::move(command_line.arguments);
                    auto outcome = co_await guard_task(agent.compact(instructions), control);
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
                case CommandKind::MODEL: {
                    auto refreshed = co_await guard_task(models.refresh(), control);
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

                    const auto selector = std::string_view(command_line.arguments);
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
                        if (!rendered(renderer.notice("[model error: " + next.error().message() + "]\n"), "cannot render model error"))
                            co_return 1;
                        continue;
                    }
                    agent.select_model(*std::move(next));
                    if (!rendered(renderer.render(ModelSelected{.name = agent.model.entry.id, .effort = agent.model.reasoning_effort}),
                                  "cannot render model selection"))
                        co_return 1;
                    continue;
                }
            }
        }

        render_error = renderer.render(PromptSubmitted{.text = prompt});
        if (render_error) {
            failure.record("cannot render submitted prompt", render_error, control);
            co_return 1;
        }
        CancellationSource task_cancellation;
        control.active_task = &task_cancellation;
        auto outcome = co_await agent.run_task(std::move(prompt), events, task_cancellation.token());
        control.active_task = nullptr;
        if (render_error) {
            failure.record("cannot render session", render_error, control);
            co_return 1;
        }
        if (outcome.is_cancelled()) {
            if (!render_error) render_error = renderer.render(TaskCancelled{});
        } else if (outcome.has_error()) {
            if (!render_error) render_error = renderer.render(TaskFailed{.message = outcome.error().message()});
        }
        if (render_error) {
            failure.record("cannot render session", render_error, control);
            co_return 1;
        }
    }
}

} // namespace

Task<i32> run_repl(Agent &agent, InterruptSource &interrupts, model::Catalog &models, std::vector<std::string> startup_notices) {
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
        auto opened = TerminalSession::open(0, terminal_output, TerminalSession::Options(false, true));
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
    TaskControl control;
    SessionFailure failure;
    i32 exit_code = 0;
    if (auto error = renderer.load_transcript(project_transcript(agent.session, *agent.tools))) {
        failure.message = "cannot hydrate session transcript: " + std::string(error.message());
        exit_code = 1;
    }
    if (exit_code == 0) {
        for (const auto &notice : startup_notices) {
            if (auto error = renderer.notice(notice)) {
                failure.message = "cannot render startup notice: " + std::string(error.message());
                exit_code = 1;
                break;
            }
        }
    }
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
        if (auto error = renderer.banner(agent.model.entry.id, agent.model.reasoning_effort, session_footer(agent))) {
            failure.record("cannot render banner", error, control);
            exit_code = 1;
        } else if (interactive) {
            PromptQueue prompts;
            ExternalEditorRequests editor_requests;
            CopyRequests copy_requests;
            SelectionCopies selection_copies;
            PromptReader reader{.prompts = &prompts};
            lighter::Event render_requested;
            renderer.set_redraw_scheduler([&render_requested] { render_requested.set(); });
#ifndef _WIN32
            auto raced = co_await WhenAny(
                repl_body(agent, reader, renderer, control, models, failure),
                signal_monitor(interrupts, renderer, control, failure, &selection_copies),
                terminal_input_loop(terminal, renderer, prompts, editor_requests, copy_requests, selection_copies, control, failure),
                render_monitor(render_requested, renderer, control, failure),
                external_editor_loop(editor_requests, terminal, renderer, control, failure),
                copy_reply_loop(copy_requests, selection_copies, agent, renderer, control, failure),
                selection_copy_loop(selection_copies, renderer, control, failure), animation_monitor(renderer, control, failure),
                suspend_monitor(suspend_controls, terminal, renderer, control, failure));
            if (raced.index() == 0) exit_code = std::get<0>(raced);
            if (raced.index() == 1) exit_code = std::get<1>(raced);
            if (raced.index() == 2) exit_code = std::get<2>(raced);
            if (raced.index() == 3) exit_code = std::get<3>(raced);
            if (raced.index() == 4) exit_code = std::get<4>(raced);
            if (raced.index() == 5) exit_code = std::get<5>(raced);
            if (raced.index() == 6) exit_code = std::get<6>(raced);
            if (raced.index() == 7) exit_code = std::get<7>(raced);
            if (raced.index() == 8) exit_code = std::get<8>(raced);
#else
            auto raced = co_await WhenAny(
                repl_body(agent, reader, renderer, control, models, failure),
                signal_monitor(interrupts, renderer, control, failure, &selection_copies),
                terminal_input_loop(terminal, renderer, prompts, editor_requests, copy_requests, selection_copies, control, failure),
                render_monitor(render_requested, renderer, control, failure),
                external_editor_loop(editor_requests, terminal, renderer, control, failure),
                copy_reply_loop(copy_requests, selection_copies, agent, renderer, control, failure),
                selection_copy_loop(selection_copies, renderer, control, failure), animation_monitor(renderer, control, failure));
            if (raced.index() == 0) exit_code = std::get<0>(raced);
            if (raced.index() == 1) exit_code = std::get<1>(raced);
            if (raced.index() == 2) exit_code = std::get<2>(raced);
            if (raced.index() == 3) exit_code = std::get<3>(raced);
            if (raced.index() == 4) exit_code = std::get<4>(raced);
            if (raced.index() == 5) exit_code = std::get<5>(raced);
            if (raced.index() == 6) exit_code = std::get<6>(raced);
            if (raced.index() == 7) exit_code = std::get<7>(raced);
#endif
            renderer.set_redraw_scheduler({});
        } else {
            PromptReader reader{.input = &pipe};
            auto raced = co_await WhenAny(repl_body(agent, reader, renderer, control, models, failure),
                                          signal_monitor(interrupts, renderer, control, failure));
            exit_code = raced.index() == 0 ? std::get<0>(raced) : std::get<1>(raced);
        }
    }

    control.active_task = nullptr;
    if (agent.session.persistence) {
        auto flushed = agent.session.persistence->flush();
        if (!flushed) {
            std::fprintf(stderr, "warning: session has an unsaved tail: %s\n", flushed.error().message().c_str());
        }
    }
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
