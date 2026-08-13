#include "tool_scheduler.h"

#include <memory>
#include <optional>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/async/runtime/timeout.h>

namespace liminal::agent::detail {

using lighter::Task;
using namespace lighter::types;

namespace {

void emit(const EventSink &events, liminal::Event event) {
    if (events) events(event);
}

struct ParallelBatch {
    lighter::Event done{true};
    usize pending = 0;

    void add() {
        if (pending++ == 0) done.reset();
    }

    void complete() {
        if (--pending == 0) done.set();
    }
};

} // namespace

struct ToolScheduler::State {
    State(const ToolSet &tools, EventSink events) : tools(&tools), events(std::move(events)) {}

    const ToolSet *tools;
    EventSink events;
    std::vector<std::string> call_ids;
    std::vector<std::optional<provider::ToolResult>> results;
    std::vector<bool> started;
    std::shared_ptr<lighter::Event> last_exclusive = std::make_shared<lighter::Event>(true);
    std::shared_ptr<ParallelBatch> parallel_batch = std::make_shared<ParallelBatch>();
    lighter::CancellationSource cancellation;
    lighter::Event all_done{true};
    usize pending = 0;
    bool accepting = true;
    bool settlement_closed = false;

    void add() {
        if (pending++ == 0) all_done.reset();
    }

    void complete() {
        if (--pending == 0) all_done.set();
    }
};

namespace {

Task<provider::ToolResult> execute_one(const std::shared_ptr<ToolScheduler::State> &state, usize slot, const provider::ToolCall &call) {
    state->started[slot] = true;
    const auto presentation = state->tools->describe(call);
    emit(state->events, ToolStarted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                        });

    auto outcome = co_await state->tools->execute(call);
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
    emit(state->events, ToolCompleted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                            .summary = state->tools->summarize(call, result),
                            .is_error = result.is_error,
                        });
    co_return result;
}

Task<provider::ToolResult> execute_parallel(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                                            std::shared_ptr<lighter::Event> predecessor) {
    co_await predecessor->wait();
    co_return co_await execute_one(state, slot, call);
}

Task<> run_parallel(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                    std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> batch) {
    auto result =
        co_await lighter::with_token(execute_parallel(state, slot, std::move(call), std::move(predecessor)), state->cancellation.token());
    if (result.has_value() && !state->settlement_closed) state->results[slot] = *std::move(result);
    batch->complete();
    state->complete();
}

Task<provider::ToolResult> execute_exclusive(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                                             std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> preceding_batch) {
    co_await predecessor->wait();
    co_await preceding_batch->done.wait();
    co_return co_await execute_one(state, slot, call);
}

Task<> run_exclusive(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                     std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> preceding_batch,
                     std::shared_ptr<lighter::Event> completed) {
    auto result = co_await lighter::with_token(
        execute_exclusive(state, slot, std::move(call), std::move(predecessor), std::move(preceding_batch)), state->cancellation.token());
    if (result.has_value() && !state->settlement_closed) state->results[slot] = *std::move(result);
    completed->set();
    state->complete();
}

std::vector<provider::ToolResult> collect_results(ToolScheduler::State &state, bool synthesize_unsettled) {
    std::vector<provider::ToolResult> results;
    results.reserve(state.results.size());
    for (usize index = 0; index < state.results.size(); ++index) {
        if (state.results[index]) {
            results.push_back(*std::move(state.results[index]));
            continue;
        }
        lighter::check(synthesize_unsettled, "settled tool call has no result");
        results.push_back({
            .call_id = state.call_ids[index],
            .content = state.started[index] ?
                           "Tool execution was cancelled before completion. Its outcome is unknown; do not assume it had no effect and do "
                           "not retry it automatically." :
                           "Tool execution was cancelled before it started. No tool action was performed.",
            .is_error = true,
        });
    }
    return results;
}

} // namespace

ToolScheduler::ToolScheduler(const ToolSet &tools, EventSink events) : state(std::make_shared<State>(tools, std::move(events))) {}

void ToolScheduler::submit(provider::ToolCall call) {
    lighter::check(state->accepting, "cannot submit a tool call after provider completion");
    const auto slot = state->results.size();
    state->call_ids.push_back(call.id);
    state->results.emplace_back();
    state->started.push_back(false);
    state->add();

    auto valid = state->tools->validate(call);
    if (!valid) {
        state->results[slot] = provider::ToolResult{
            .call_id = call.id,
            .content = "Error: " + valid.error().message(),
            .is_error = true,
        };
        state->complete();
        return;
    }

    if (state->tools->execution_mode(call.name) == ToolExecutionMode::PARALLEL) {
        auto batch = state->parallel_batch;
        batch->add();
        lighter::EventLoop::current().schedule(run_parallel(state, slot, std::move(call), state->last_exclusive, std::move(batch)));
        return;
    }

    auto completed = std::make_shared<lighter::Event>();
    lighter::EventLoop::current().schedule(
        run_exclusive(state, slot, std::move(call), state->last_exclusive, state->parallel_batch, completed));
    state->last_exclusive = std::move(completed);
    state->parallel_batch = std::make_shared<ParallelBatch>();
}

void ToolScheduler::finish_accepting() { state->accepting = false; }

Task<std::vector<provider::ToolResult>> ToolScheduler::finish() {
    finish_accepting();
    co_await state->all_done.wait();
    state->settlement_closed = true;
    co_return collect_results(*state, false);
}

Task<std::vector<provider::ToolResult>, Error> ToolScheduler::cancel_and_finish(std::chrono::milliseconds grace_period) {
    finish_accepting();
    state->cancellation.cancel();
    auto settled = co_await lighter::with_timeout(state->all_done.wait(), grace_period);
    if (settled.has_error() && settled.error() != lighter::Error::k_connection_timed_out) {
        co_await lighter::fail(Error::protocol("cannot settle cancelled tool calls: " + std::string(settled.error().message())));
    }
    state->settlement_closed = true;
    state->events = {};
    co_return collect_results(*state, true);
}

} // namespace liminal::agent::detail
