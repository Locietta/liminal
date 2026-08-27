#pragma once

#include "tool_planner.h"

#include <chrono>
#include <memory>
#include <vector>

#include <lighter/async/runtime/task.h>

#include <liminal/event.h>

namespace liminal::agent::detail {

/// Executes an already admitted batch. It owns ordering, cancellation, and
/// exactly-one-outcome settlement, but has no context or budgeting policy.
struct ToolExecutor {
    struct State;

    ToolExecutor(const ToolSet &tools, ToolBatchPlan plan, EventSink events, ActivityScope activity_scope);

    lighter::Task<std::vector<ToolOutcome>> finish();
    lighter::Task<std::vector<ToolOutcome>> cancel_and_finish(std::chrono::milliseconds grace_period);

private:
    std::shared_ptr<State> state;
};

} // namespace liminal::agent::detail
