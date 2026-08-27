#include "tool_scheduler.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/async/runtime/timeout.h>

namespace liminal::agent::detail {

using lighter::Task;
using namespace lighter::types;

namespace {

constexpr usize k_min_tool_result_bytes = 64;

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
    State(const ToolSet &tools, std::copyable_function<ToolExecutionContext(std::span<const std::string>)> execution_context,
          EventSink events, ActivityScope activity_scope)
        : tools(&tools), execution_context(std::move(execution_context)), events(std::move(events)), activity_scope(activity_scope) {}

    const ToolSet *tools;
    std::copyable_function<ToolExecutionContext(std::span<const std::string>)> execution_context;
    EventSink events;
    ActivityScope activity_scope;
    std::vector<std::string> call_ids;
    std::vector<provider::ToolCall> calls;
    std::vector<std::optional<provider::ToolResult>> results;
    std::vector<bool> started;
    std::vector<usize> output_budgets;
    std::shared_ptr<lighter::Event> last_exclusive = std::make_shared<lighter::Event>(true);
    std::shared_ptr<ParallelBatch> parallel_batch = std::make_shared<ParallelBatch>();
    lighter::CancellationSource cancellation;
    lighter::Event all_done{true};
    usize pending = 0;
    bool accepting = true;
    bool settlement_closed = false;
    bool insufficient_capacity = false;

    void add() {
        if (pending++ == 0) all_done.reset();
    }

    void complete() {
        if (--pending == 0) all_done.set();
    }
};

namespace {

void emit_completed(const std::shared_ptr<ToolScheduler::State> &state, const provider::ToolCall &call,
                    const provider::ToolResult &result) {
    const auto presentation = state->tools->describe(call);
    emit(state->events, ToolCompleted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                            .summary = state->tools->summarize(call, result),
                            .is_error = result.is_error,
                            .activity_scope = state->activity_scope,
                        });
}

Task<provider::ToolResult, Error> execute_one(const std::shared_ptr<ToolScheduler::State> &state, usize slot,
                                              const provider::ToolCall &call) {
    state->started[slot] = true;
    const auto presentation = state->tools->describe(call);
    emit(state->events, ToolStarted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                            .activity_scope = state->activity_scope,
                        });

    const ToolExecutionContext execution_context{.max_output_bytes = state->output_budgets[slot]};
    auto result = co_await state->tools->execute(call, execution_context).or_fail();
    sanitize_tool_result(result, state->output_budgets[slot]);
    emit_completed(state, call, result);
    co_return result;
}

Task<provider::ToolResult, Error> execute_parallel(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                                                   std::shared_ptr<lighter::Event> predecessor) {
    co_await predecessor->wait();
    co_return co_await execute_one(state, slot, call);
}

Task<> run_parallel(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                    std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> batch) {
    auto result = co_await lighter::with_token(execute_parallel(state, slot, call, std::move(predecessor)), state->cancellation.token());
    if (!state->settlement_closed) {
        if (result.has_value()) {
            state->results[slot] = *std::move(result);
        } else if (result.has_error()) {
            provider::ToolResult failure{
                .call_id = state->call_ids[slot],
                .content = "Error: " + std::string(result.error().message()),
                .is_error = true,
            };
            sanitize_tool_result(failure, state->output_budgets[slot]);
            emit_completed(state, call, failure);
            state->results[slot] = std::move(failure);
        }
    }
    batch->complete();
    state->complete();
}

Task<provider::ToolResult, Error> execute_exclusive(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                                                    std::shared_ptr<lighter::Event> predecessor,
                                                    std::shared_ptr<ParallelBatch> preceding_batch) {
    co_await predecessor->wait();
    co_await preceding_batch->done.wait();
    co_return co_await execute_one(state, slot, call);
}

Task<> run_exclusive(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                     std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> preceding_batch,
                     std::shared_ptr<lighter::Event> completed) {
    auto result = co_await lighter::with_token(execute_exclusive(state, slot, call, std::move(predecessor), std::move(preceding_batch)),
                                               state->cancellation.token());
    if (!state->settlement_closed) {
        if (result.has_value()) {
            state->results[slot] = *std::move(result);
        } else if (result.has_error()) {
            provider::ToolResult failure{
                .call_id = state->call_ids[slot],
                .content = "Error: " + std::string(result.error().message()),
                .is_error = true,
            };
            sanitize_tool_result(failure, state->output_budgets[slot]);
            emit_completed(state, call, failure);
            state->results[slot] = std::move(failure);
        }
    }
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
        provider::ToolResult result{
            .call_id = state.call_ids[index],
            .content = state.started[index] ?
                           "Tool execution was cancelled before completion. Its outcome is unknown; do not assume it had no effect and do "
                           "not retry it automatically." :
                           "Tool execution was cancelled before it started. No tool action was performed.",
            .is_error = true,
        };
        sanitize_tool_result(result, state.output_budgets[index]);
        results.push_back(std::move(result));
    }
    return results;
}

void schedule_call(const std::shared_ptr<ToolScheduler::State> &state, usize slot) {
    state->add();
    auto call = std::move(state->calls[slot]);
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

void commit_calls(const std::shared_ptr<ToolScheduler::State> &state, bool dispatch) {
    if (!state->accepting) return;
    state->accepting = false;

    auto remaining = std::min(state->execution_context(state->call_ids).max_output_bytes, k_max_tool_output_bytes);
    if (!state->call_ids.empty() && remaining / state->call_ids.size() < k_min_tool_result_bytes) {
        state->insufficient_capacity = true;
        return;
    }
    for (usize index = 0; index < state->call_ids.size(); ++index) {
        const auto calls_left = state->call_ids.size() - index;
        const auto budget = remaining / calls_left;
        state->output_budgets[index] = budget;
        remaining -= budget;
        if (state->results[index]) sanitize_tool_result(*state->results[index], budget);
    }
    if (!dispatch) return;
    for (usize index = 0; index < state->calls.size(); ++index) {
        if (!state->results[index]) schedule_call(state, index);
    }
}

} // namespace

ToolScheduler::ToolScheduler(const ToolSet &tools,
                             std::copyable_function<ToolExecutionContext(std::span<const std::string>)> execution_context, EventSink events,
                             ActivityScope activity_scope)
    : state(std::make_shared<State>(tools, std::move(execution_context), std::move(events), activity_scope)) {}

void ToolScheduler::submit(provider::ToolCall call) {
    lighter::check(state->accepting, "cannot submit a tool call after provider completion");
    const auto slot = state->results.size();
    state->call_ids.push_back(call.id);
    state->calls.push_back(std::move(call));
    state->results.emplace_back();
    state->started.push_back(false);
    state->output_budgets.push_back(0);

    auto valid = state->tools->validate(state->calls.back());
    if (!valid) {
        state->results[slot] = provider::ToolResult{
            .call_id = state->call_ids.back(),
            .content = "Error: " + valid.error().message(),
            .is_error = true,
        };
    }
}

void ToolScheduler::finish_accepting() { commit_calls(state, true); }

bool ToolScheduler::has_output_capacity() const {
    lighter::check(!state->accepting, "tool output capacity is unknown before provider completion");
    return !state->insufficient_capacity;
}

Task<std::vector<provider::ToolResult>> ToolScheduler::finish() {
    finish_accepting();
    lighter::check(!state->insufficient_capacity, "cannot finish a tool batch without output capacity");
    co_await state->all_done.wait();
    // The runners signal all_done immediately before returning. Let those
    // detached coroutine frames retire before closing settlement state.
    co_await lighter::yield();
    state->settlement_closed = true;
    co_return collect_results(*state, false);
}

Task<std::vector<provider::ToolResult>, Error> ToolScheduler::cancel_and_finish(std::chrono::milliseconds grace_period) {
    commit_calls(state, false);
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
