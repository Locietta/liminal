#pragma once

#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <liminal/provider/common.h>
#include <liminal/tools/tools.h>

namespace liminal::agent::detail {

struct PlannedToolCall {
    provider::ToolCall call;
    ToolExecutionMode mode = ToolExecutionMode::EXCLUSIVE;
    ToolOutputGrant grant{.receipt_bytes = 0, .payload_bytes = 0};
    std::optional<ToolOutcome> immediate_outcome;
    std::move_only_function<lighter::Task<ToolOutcome, Error>(ToolOutputGrant)> execute;
};

struct ToolBatchPlan {
    std::vector<PlannedToolCall> calls;
};

struct ToolBatchRejected {
    std::vector<ToolOutcome> outcomes;
};

using ToolBatchAdmission = std::variant<ToolBatchPlan, ToolBatchRejected>;

struct ToolBatchPlanner {
    ToolBatchPlanner(const ToolSet &tools, std::copyable_function<std::optional<usize>(std::span<const ToolOutcome>)> payload_capacity);

    ToolBatchAdmission plan(std::vector<provider::ToolCall> calls);

    const ToolSet *tools;
    std::copyable_function<std::optional<usize>(std::span<const ToolOutcome>)> payload_capacity;
};

} // namespace liminal::agent::detail
