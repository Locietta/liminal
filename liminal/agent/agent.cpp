#include "agent.h"

#include <string>
#include <utility>
#include <vector>

#include <lighter/async/runtime/when.h>
#include <lighter/async/runtime/sync.h>

namespace liminal {

using lighter::fail;
using lighter::Task;
using lighter::WhenAll;

namespace {

void emit(const EventSink &events, Event event) {
    if (events) {
        events(event);
    }
}

/// Runs one tool call, converting infrastructure failures into is_error
/// results so one bad call never aborts its siblings. Cancellation still
/// unwinds normally.
struct ToolPermit {
    lighter::Semaphore *slots;

    ~ToolPermit() { slots->release(); }
};

Task<provider::ToolResult> execute_one(const ToolSet &tools, const provider::ToolCall &call, const EventSink &events,
                                       lighter::Semaphore &slots) {
    co_await slots.acquire();
    ToolPermit permit{&slots};
    auto outcome = co_await tools.execute(call);
    provider::ToolResult result;
    if (!outcome) {
        result = {
            .call_id = call.id,
            .content = "Error: " + outcome.error().message(),
            .is_error = true,
        };
    } else {
        result = *std::move(outcome);
    }
    emit(events, ToolCompleted{.call_id = call.id, .name = call.name, .is_error = result.is_error});
    co_return result;
}

} // namespace

Task<void, Error> Agent::run_turn(std::string prompt, EventSink events) {
    // Transactional: staged history only replaces committed history after a
    // complete terminal response. The UI transcript intentionally remains.
    auto staged = history;
    provider::append_user(staged, std::move(prompt));

    provider::StreamCallbacks stream{
        .on_text_delta = [&events](std::string_view text) { emit(events, AssistantTextDelta{.text = std::string(text)}); },
    };

    constexpr i32 k_max_iterations = 32;
    usize tool_calls_used = 0;
    lighter::Semaphore tool_slots(static_cast<isize>(tools->policy.max_parallel_calls));
    for (i32 iteration = 0; iteration < k_max_iterations; ++iteration) {
        auto response = co_await model.handle->complete(staged, tools->definitions(), stream).or_fail();
        auto calls = provider::tool_calls(response);
        emit(events, AssistantSegmentCompleted{});

        switch (response.stop) {
            case provider::StopKind::DONE:
                provider::append_response(staged, std::move(response));
                history = std::move(staged);
                emit(events, TurnCompleted{});
                co_return;
            case provider::StopKind::NEEDS_TOOL_RESULTS: break;
            case provider::StopKind::TRUNCATED:
                co_await fail(Error::protocol("response truncated (" + response.stop_detail + "); raise max_tokens"));
            case provider::StopKind::CONTEXT_EXHAUSTED: co_await fail(Error::protocol("context window exhausted; try /compact"));
            case provider::StopKind::REFUSED: co_await fail(Error::protocol("the model refused to continue this conversation"));
            case provider::StopKind::OTHER: co_await fail(Error::protocol("unsupported stop reason: " + response.stop_detail));
        }
        if (calls.empty()) {
            co_await fail(Error::protocol("provider requested tool results without any tool calls"));
        }
        if (calls.size() > tools->policy.max_calls_per_turn - tool_calls_used) {
            co_await fail(
                Error::tool("tool call budget exceeded (maximum " + std::to_string(tools->policy.max_calls_per_turn) + " per turn)"));
        }
        tool_calls_used += calls.size();

        std::vector<Task<provider::ToolResult>> pending;
        pending.reserve(calls.size());
        for (const auto *call : calls) {
            emit(events, ToolStarted{.call_id = call->id, .name = call->name});
            pending.push_back(execute_one(*tools, *call, events, tool_slots));
        }
        auto joined = co_await WhenAll(std::move(pending));

        provider::append_response(staged, std::move(response));
        std::vector<provider::ToolResult> results;
        results.reserve(joined.size());
        for (auto &result : joined) {
            results.push_back(std::move(result));
        }
        provider::append_tool_results(staged, std::move(results));
    }
    co_await fail(Error::protocol("exceeded " + std::to_string(k_max_iterations) + " tool iterations in one turn"));
}

Task<void, Error> Agent::compact(std::string_view instructions) {
    co_return co_await model.handle->compact(history, instructions).or_fail();
}

} // namespace liminal
