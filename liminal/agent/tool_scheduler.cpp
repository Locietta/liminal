#include "tool_scheduler.h"

#include <memory>
#include <optional>
#include <utility>

#include <lighter/async/async.h>

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
    std::vector<std::optional<provider::ToolResult>> results;
    std::shared_ptr<lighter::Event> last_exclusive = std::make_shared<lighter::Event>(true);
    std::shared_ptr<ParallelBatch> parallel_batch = std::make_shared<ParallelBatch>();
    lighter::Event all_done{true};
    usize pending = 0;

    void add() {
        if (pending++ == 0) all_done.reset();
    }

    void complete() {
        if (--pending == 0) all_done.set();
    }
};

namespace {

Task<provider::ToolResult> execute_one(const std::shared_ptr<ToolScheduler::State> &state, const provider::ToolCall &call) {
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

Task<> run_parallel(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                    std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> batch) {
    co_await predecessor->wait();
    state->results[slot] = co_await execute_one(state, call);
    batch->complete();
    state->complete();
}

Task<> run_exclusive(std::shared_ptr<ToolScheduler::State> state, usize slot, provider::ToolCall call,
                     std::shared_ptr<lighter::Event> predecessor, std::shared_ptr<ParallelBatch> preceding_batch,
                     std::shared_ptr<lighter::Event> completed) {
    co_await predecessor->wait();
    co_await preceding_batch->done.wait();
    state->results[slot] = co_await execute_one(state, call);
    completed->set();
    state->complete();
}

} // namespace

ToolScheduler::ToolScheduler(const ToolSet &tools, EventSink events) : state(std::make_shared<State>(tools, std::move(events))) {}

void ToolScheduler::submit(provider::ToolCall call) {
    const auto slot = state->results.size();
    state->results.emplace_back();
    state->add();

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

Task<std::vector<provider::ToolResult>> ToolScheduler::finish() {
    co_await state->all_done.wait();
    std::vector<provider::ToolResult> results;
    results.reserve(state->results.size());
    for (auto &result : state->results) {
        if (result) results.push_back(*std::move(result));
    }
    co_return results;
}

} // namespace liminal::agent::detail
