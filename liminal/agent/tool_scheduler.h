#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lighter/async/runtime/task.h>

#include <liminal/event.h>
#include <liminal/provider/common.h>
#include <liminal/tools/tools.h>

namespace liminal::agent::detail {

/// Provider-call-scoped scheduler. Calls are accepted in model order, then
/// dispatched after the complete batch receives immutable output allowances.
/// Parallel-safe calls overlap; exclusive calls are barriers on both sides.
struct ToolScheduler {
    struct State;

    ToolScheduler(const ToolSet &tools, std::copyable_function<ToolExecutionContext(std::span<const std::string>)> execution_context,
                  EventSink events, ActivityScope activity_scope);

    void submit(provider::ToolCall call);
    void finish_accepting();
    bool has_output_capacity() const;
    lighter::Task<std::vector<provider::ToolResult>> finish();
    lighter::Task<std::vector<provider::ToolResult>, Error> cancel_and_finish(std::chrono::milliseconds grace_period);

private:
    std::shared_ptr<State> state;
};

} // namespace liminal::agent::detail
