#include "tool_executor.h"

#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/async/runtime/timeout.h>

#include <liminal/text.h>

namespace liminal::agent::detail {

using lighter::Task;
using namespace lighter::types;

namespace {

constexpr usize k_diagnostic_bytes = 4 * 1024;

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

struct ToolExecutor::State {
    State(const ToolSet &tools, ToolBatchPlan plan, EventSink events, ActivityScope activity_scope)
        : tools(&tools), events(std::move(events)), activity_scope(activity_scope) {
        calls.reserve(plan.calls.size());
        modes.reserve(plan.calls.size());
        grants.reserve(plan.calls.size());
        executions.reserve(plan.calls.size());
        results.reserve(plan.calls.size());
        started.reserve(plan.calls.size());
        for (auto &planned : plan.calls) {
            calls.push_back(std::move(planned.call));
            modes.push_back(planned.mode);
            grants.push_back(planned.grant);
            executions.push_back(std::move(planned.execute));
            results.push_back(std::move(planned.immediate_outcome));
            started.push_back(false);
        }
    }

    const ToolSet *tools;
    EventSink events;
    ActivityScope activity_scope;
    std::vector<provider::ToolCall> calls;
    std::vector<ToolExecutionMode> modes;
    std::vector<ToolOutputGrant> grants;
    std::vector<std::move_only_function<Task<ToolOutcome, Error>(ToolOutputGrant)>> executions;
    std::vector<std::optional<ToolOutcome>> results;
    std::vector<bool> started;
    std::shared_ptr<lighter::Event> last_exclusive = std::make_shared<lighter::Event>(true);
    std::shared_ptr<ParallelBatch> parallel_batch = std::make_shared<ParallelBatch>();
    lighter::CancellationSource cancellation;
    lighter::Event all_done{true};
    usize pending = 0;
    bool settlement_closed = false;

    void add() {
        if (pending++ == 0) all_done.reset();
    }

    void complete() {
        if (--pending == 0) all_done.set();
    }
};

namespace {

void emit_completed(const std::shared_ptr<ToolExecutor::State> &state, usize slot, const ToolOutcome &outcome) {
    const auto &call = state->calls[slot];
    const auto presentation = state->tools->describe(call);
    emit(state->events, ToolCompleted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                            .summary = state->tools->summarize(call, outcome),
                            .is_error = tool_outcome_is_error(outcome.kind),
                            .activity_scope = state->activity_scope,
                        });
}

Task<ToolOutcome, Error> execute_one(const std::shared_ptr<ToolExecutor::State> &state, usize slot) {
    state->started[slot] = true;
    const auto &call = state->calls[slot];
    const auto presentation = state->tools->describe(call);
    emit(state->events, ToolStarted{
                            .call_id = call.id,
                            .name = call.name,
                            .description = presentation.description,
                            .command = presentation.command,
                            .activity_scope = state->activity_scope,
                        });
    auto outcome = co_await state->executions[slot](state->grants[slot]).or_fail();
    outcome.call_id = call.id;
    finalize_tool_outcome(outcome, state->grants[slot]);
    emit_completed(state, slot, outcome);
    co_return outcome;
}

Task<ToolOutcome, Error> execute_parallel(std::shared_ptr<ToolExecutor::State> state, usize slot,
                                          std::shared_ptr<lighter::Event> predecessor) {
    co_await predecessor->wait();
    co_return co_await execute_one(state, slot);
}

void store_execution_error(const std::shared_ptr<ToolExecutor::State> &state, usize slot, std::string_view detail) {
    ToolOutcome outcome{
        .call_id = state->calls[slot].id,
        .kind = ToolOutcomeKind::OUTCOME_UNKNOWN,
        .receipt = "reason: execution_error",
        .payload = bounded_utf8(detail, k_diagnostic_bytes),
    };
    finalize_tool_outcome(outcome, state->grants[slot]);
    emit_completed(state, slot, outcome);
    state->results[slot] = std::move(outcome);
}

Task<> run_parallel(std::shared_ptr<ToolExecutor::State> state, usize slot, std::shared_ptr<lighter::Event> predecessor,
                    std::shared_ptr<ParallelBatch> batch) {
    auto result = co_await lighter::with_token(execute_parallel(state, slot, std::move(predecessor)), state->cancellation.token());
    if (!state->settlement_closed) {
        if (result.has_value()) {
            state->results[slot] = *std::move(result);
        } else if (result.has_error()) {
            store_execution_error(state, slot, result.error().message());
        }
    }
    batch->complete();
    state->complete();
}

Task<ToolOutcome, Error> execute_exclusive(std::shared_ptr<ToolExecutor::State> state, usize slot,
                                           std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> preceding_batch) {
    co_await predecessor->wait();
    co_await preceding_batch->done.wait();
    co_return co_await execute_one(state, slot);
}

Task<> run_exclusive(std::shared_ptr<ToolExecutor::State> state, usize slot, std::shared_ptr<lighter::Event> predecessor,
                     std::shared_ptr<ParallelBatch> preceding_batch, std::shared_ptr<lighter::Event> completed) {
    auto result = co_await lighter::with_token(execute_exclusive(state, slot, std::move(predecessor), std::move(preceding_batch)),
                                               state->cancellation.token());
    if (!state->settlement_closed) {
        if (result.has_value()) {
            state->results[slot] = *std::move(result);
        } else if (result.has_error()) {
            store_execution_error(state, slot, result.error().message());
        }
    }
    completed->set();
    state->complete();
}

void schedule_call(const std::shared_ptr<ToolExecutor::State> &state, usize slot) {
    if (state->results[slot]) return;
    state->add();
    if (state->modes[slot] == ToolExecutionMode::PARALLEL) {
        auto batch = state->parallel_batch;
        batch->add();
        lighter::EventLoop::current().schedule(run_parallel(state, slot, state->last_exclusive, std::move(batch)));
        return;
    }
    auto completed = std::make_shared<lighter::Event>();
    lighter::EventLoop::current().schedule(run_exclusive(state, slot, state->last_exclusive, state->parallel_batch, completed));
    state->last_exclusive = std::move(completed);
    state->parallel_batch = std::make_shared<ParallelBatch>();
}

std::vector<ToolOutcome> collect_outcomes(ToolExecutor::State &state, bool synthesize_unsettled) {
    std::vector<ToolOutcome> outcomes;
    outcomes.reserve(state.results.size());
    for (usize index = 0; index < state.results.size(); ++index) {
        if (state.results[index]) {
            outcomes.push_back(*std::move(state.results[index]));
            continue;
        }
        lighter::check(synthesize_unsettled, "settled tool call has no outcome");
        ToolOutcome outcome{
            .call_id = state.calls[index].id,
            .kind = state.started[index] ? ToolOutcomeKind::OUTCOME_UNKNOWN : ToolOutcomeKind::NOT_STARTED,
            .receipt = state.started[index] ? "reason: cancelled_during_execution" : "reason: cancelled_before_execution",
        };
        finalize_tool_outcome(outcome, state.grants[index]);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

} // namespace

ToolExecutor::ToolExecutor(const ToolSet &tools, ToolBatchPlan plan, EventSink events, ActivityScope activity_scope)
    : state(std::make_shared<State>(tools, std::move(plan), std::move(events), activity_scope)) {
    for (usize index = 0; index < state->calls.size(); ++index) {
        if (state->results[index]) emit_completed(state, index, *state->results[index]);
        schedule_call(state, index);
    }
}

Task<std::vector<ToolOutcome>> ToolExecutor::finish() {
    co_await state->all_done.wait();
    co_await lighter::yield();
    state->settlement_closed = true;
    co_return collect_outcomes(*state, false);
}

Task<std::vector<ToolOutcome>> ToolExecutor::cancel_and_finish(std::chrono::milliseconds grace_period) {
    state->cancellation.cancel();
    std::ignore = co_await lighter::with_timeout(state->all_done.wait(), grace_period);
    state->settlement_closed = true;
    state->events = {};
    co_return collect_outcomes(*state, true);
}

} // namespace liminal::agent::detail
