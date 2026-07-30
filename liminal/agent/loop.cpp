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
Task<anthropic::ToolResultBlock> execute_one(const ToolSet &tools, const anthropic::ToolUseBlock &call) {
    auto outcome = co_await tools.execute(call);
    if (!outcome) {
        co_return anthropic::ToolResultBlock{
            .tool_use_id = call.id,
            .content = "Error: " + outcome.error().message(),
            .is_error = true,
        };
    }
    co_return *std::move(outcome);
}

} // namespace

Task<void, Error> Agent::run_turn(std::string prompt, const anthropic::StreamCallbacks &callbacks) {
    // Transactional: staged history only replaces the committed history after
    // a complete terminal response, so cancellation and errors can simply
    // drop the partial turn without leaving tool_use/tool_result imbalances.
    auto staged = history;
    staged.push_back(anthropic::user_text(std::move(prompt)));

    constexpr i32 k_max_iterations = 32;
    for (i32 iteration = 0; iteration < k_max_iterations; ++iteration) {
        anthropic::MessageRequest request{
            .model = model,
            .max_tokens = max_tokens,
            .messages = staged,
            .tools = tools->definitions(),
        };
        auto response = co_await client->create_message(request, callbacks).or_fail();

        if (response.stop_reason == "end_turn" || response.stop_reason == "stop_sequence") {
            staged.push_back({.role = std::string(anthropic::k_role_assistant), .content = std::move(response.content)});
            history = std::move(staged);
            co_return;
        }
        if (response.stop_reason != "tool_use") {
            co_await fail(Error::protocol("turn ended with unhandled stop_reason: " + response.stop_reason));
        }

        // The full content array - thinking blocks included - must be echoed
        // back for a valid continuation.
        staged.push_back({.role = std::string(anthropic::k_role_assistant), .content = std::move(response.content)});

        std::vector<Task<anthropic::ToolResultBlock>> pending;
        for (const auto &block : staged.back().content) {
            if (const auto *call = std::get_if<anthropic::ToolUseBlock>(&block)) {
                std::printf("\n[running tool: %s]\n", call->name.c_str());
                pending.push_back(execute_one(*tools, *call));
            }
        }
        if (pending.empty()) {
            co_await fail(Error::protocol("stop_reason tool_use without tool_use blocks"));
        }

        auto joined = co_await WhenAll(std::move(pending));

        // All results travel in a single user message, in call order.
        anthropic::Message tool_message{.role = std::string(anthropic::k_role_user)};
        for (auto &result : joined) {
            tool_message.content.push_back(std::move(result));
        }
        staged.push_back(std::move(tool_message));
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

Task<i32> repl_body(Agent &agent, LineReader &reader, TurnControl &control) {
    anthropic::StreamCallbacks callbacks{
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

Task<i32> run_repl(Agent &agent, InterruptSource &interrupts) {
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

} // namespace liminal
