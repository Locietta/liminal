#include "loop.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/interrupt.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/cancellation.h>

#include "liminal/provider/anthropic.h"
#include "liminal/provider/openai.h"

namespace liminal {

using lighter::CancellationSource;
using lighter::Console;
using lighter::fail;
using lighter::InterruptSource;
using lighter::Pipe;
using lighter::Stream;
using lighter::Task;
using lighter::WhenAll;
using lighter::WhenAny;

namespace {

/// Runs one tool call, converting infrastructure failures into is_error
/// results so one bad call never aborts its siblings. Cancellation still
/// unwinds normally - it is not an error.
Task<provider::ToolResult> execute_one(const ToolSet &tools, const provider::ToolCall &call) {
    auto outcome = co_await tools.execute(call);
    if (!outcome) {
        co_return provider::ToolResult{
            .call_id = call.id,
            .content = "Error: " + outcome.error().message(),
            .is_error = true,
        };
    }
    co_return *std::move(outcome);
}

} // namespace

template <typename Client>
Task<void, Error> Agent<Client>::run_turn(std::string prompt, const provider::StreamCallbacks &callbacks) {
    // Transactional: staged history only replaces the committed history after
    // a complete terminal response, so cancellation and errors can simply
    // drop the partial turn without leaving tool_use/tool_result imbalances.
    auto staged = history;
    Client::append_user(staged, std::move(prompt));

    constexpr i32 k_max_iterations = 32;
    for (i32 iteration = 0; iteration < k_max_iterations; ++iteration) {
        auto response = co_await client->create_message(model, max_tokens, staged, tools->definitions(), callbacks).or_fail();
        auto calls = Client::tool_calls(response);

        if (Client::is_terminal(response)) {
            Client::append_response(staged, std::move(response));
            history = std::move(staged);
            co_return;
        }
        if (!Client::requires_tool_results(response)) {
            co_await fail(Error::protocol("provider returned an unsupported non-terminal response"));
        }
        if (calls.empty()) {
            co_await fail(Error::protocol("provider requested continuation without any tool calls"));
        }

        std::vector<Task<provider::ToolResult>> pending;
        pending.reserve(calls.size());
        for (const auto *call : calls) {
            std::printf("\n[running tool: %s]\n", call->name.c_str());
            pending.push_back(execute_one(*tools, *call));
        }
        auto joined = co_await WhenAll(std::move(pending));

        // Preserve the complete provider response before its tool outputs.
        Client::append_response(staged, std::move(response));
        std::vector<provider::ToolResult> results;
        results.reserve(joined.size());
        for (auto &result : joined) {
            results.push_back(std::move(result));
        }
        Client::append_tool_results(staged, std::move(results));
    }
    co_await fail(Error::protocol("exceeded " + std::to_string(k_max_iterations) + " tool iterations in one turn"));
}

namespace {

/// Splits raw stdin chunks into lines. No editing, no history - v1.
struct LineReader {
    Stream *input = nullptr;
    std::string buffered;
    bool eof = false;

    Task<std::optional<std::string>, lighter::Error> next_line() {
        while (true) {
            if (auto pos = buffered.find('\n'); pos != std::string::npos) {
                auto line = buffered.substr(0, pos);
                buffered.erase(0, pos + 1);
                co_return finish_line(std::move(line));
            }
            if (eof) {
                if (buffered.empty()) {
                    co_return std::nullopt;
                }
                co_return finish_line(std::exchange(buffered, {}));
            }
            auto chunk = co_await input->read_chunk();
            if (!chunk) {
                // Pipe/console EOF arrives on the error channel.
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
    static std::string finish_line(std::string line) {
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

/// Watches fatal signals for the REPL's lifetime. First Ctrl-C cancels the
/// in-flight turn (and keeps watching); with no turn active it returns, which
/// makes WhenAny cancel the REPL body blocked on stdin. A second Ctrl-C
/// hard-exits - the escape hatch when cleanup itself wedges.
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

template <typename Client>
Task<i32> repl_body(Agent<Client> &agent, LineReader &reader, TurnControl &control) {
    provider::StreamCallbacks callbacks{
        .on_text_delta = [](std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); },
    };

    while (true) {
        std::fputs("\n> ", stdout);
        std::fflush(stdout);

        auto line = co_await reader.next_line();
        if (!line || !line->has_value()) {
            co_return 0; // read error or EOF: quit cleanly
        }
        auto &prompt = **line;
        if (prompt.empty()) {
            continue;
        }
        if (prompt == "/quit" || prompt == "/exit") {
            co_return 0;
        }
        if (prompt == "/compact" || prompt.starts_with("/compact ")) {
            if constexpr (requires { agent.client->compact_history(agent.model, agent.history, std::string{}); }) {
                auto instructions = prompt == "/compact" ? std::string(k_default_compact_instructions) :
                                                           prompt.substr(std::string_view("/compact ").size());
                auto compacted = co_await agent.client->compact_history(agent.model, agent.history, std::move(instructions));
                if (compacted) {
                    std::fputs("[history compacted]\n", stdout);
                } else {
                    std::fprintf(stdout, "[compact error: %s]\n", compacted.error().message().c_str());
                }
            } else {
                std::fputs("[this provider does not support remote compaction]\n", stdout);
            }
            continue;
        }

        CancellationSource turn_source;
        control.active_turn = &turn_source;
        auto outcome = co_await with_token(agent.run_turn(std::move(prompt), callbacks), turn_source.token());
        control.active_turn = nullptr;

        if (outcome.is_cancelled()) {
            std::fputs("\n[turn cancelled; discarded from history - command side effects may remain]\n", stdout);
        } else if (outcome.has_error()) {
            std::fprintf(stdout, "\n[error: %s]\n", outcome.error().message().c_str());
        } else {
            std::fputs("\n", stdout);
        }
    }
}

} // namespace

template <typename Client>
Task<i32> run_repl(Agent<Client> &agent, InterruptSource &interrupts) {
    LineReader reader;
    Console console;
    Pipe pipe;
    if (lighter::guess_handle(0) == lighter::HandleType::TTY) {
        auto opened = Console::open(0, Console::Options(/*readable=*/true));
        if (!opened) {
            std::fprintf(stderr, "cannot open stdin console: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
        console = *std::move(opened);
        reader.input = &console;
    } else {
        auto opened = Pipe::open(0);
        if (!opened) {
            std::fprintf(stderr, "cannot open stdin pipe: %s\n", std::string(opened.error().message()).c_str());
            co_return 1;
        }
        pipe = *std::move(opened);
        reader.input = &pipe;
    }

    TurnControl control;
    auto raced = co_await WhenAny(repl_body(agent, reader, control), signal_monitor(interrupts, control));
    if (raced.index() == 0) {
        co_return std::get<0>(raced);
    }
    co_return 130; // idle Ctrl-C
}

template struct Agent<anthropic::Client>;
template struct Agent<openai::Client>;
template Task<i32> run_repl(Agent<anthropic::Client> &, InterruptSource &);
template Task<i32> run_repl(Agent<openai::Client> &, InterruptSource &);

} // namespace liminal
