#pragma once

#include <memory>
#include <vector>

#include <lighter/async/runtime/task.h>

#include <liminal/event.h>
#include <liminal/provider/common.h>
#include <liminal/tools/tools.h>

namespace liminal::agent::detail {

/// Provider-call-scoped online scheduler. Calls are accepted in model order
/// and may begin while the provider is still producing later output items.
/// Parallel-safe calls overlap; exclusive calls are barriers on both sides.
struct ToolScheduler {
    struct State;

    ToolScheduler(const ToolSet &tools, EventSink events);

    void submit(provider::ToolCall call);
    lighter::Task<std::vector<provider::ToolResult>> finish();

private:
    std::shared_ptr<State> state;
};

} // namespace liminal::agent::detail
