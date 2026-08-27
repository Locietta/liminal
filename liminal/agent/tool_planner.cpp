#include "tool_planner.h"

#include <algorithm>
#include <string>
#include <utility>

#include <liminal/text.h>

namespace liminal::agent::detail {

namespace {

constexpr usize k_executor_receipt_bytes = 128;
constexpr usize k_diagnostic_bytes = 4 * 1024;

usize projected_receipt_bytes(usize receipt_bytes) { return receipt_bytes + k_tool_outcome_payload_framing.size(); }

ToolOutcome planning_failure(std::string call_id, ToolOutcomeKind kind, std::string reason, std::string detail = {}) {
    return {
        .call_id = std::move(call_id),
        .kind = kind,
        .receipt = "reason: " + std::move(reason),
        .payload = bounded_utf8(detail, k_diagnostic_bytes),
    };
}

} // namespace

ToolBatchPlanner::ToolBatchPlanner(const ToolSet &tools,
                                   std::copyable_function<std::optional<usize>(std::span<const ToolOutcome>)> payload_capacity)
    : tools(&tools), payload_capacity(std::move(payload_capacity)) {}

ToolBatchAdmission ToolBatchPlanner::plan(std::vector<provider::ToolCall> calls) {
    ToolBatchPlan plan;
    plan.calls.reserve(calls.size());
    std::vector<ToolOutcome> projected;
    projected.reserve(calls.size());
    usize minimum_payload_bytes = 0;

    for (auto &call : calls) {
        auto call_id = call.id;
        auto prepared = tools->prepare(std::move(call));
        if (!prepared) {
            auto outcome = planning_failure(std::move(call_id), ToolOutcomeKind::FAILED, "validation_failed", prepared.error().message());
            const auto receipt_bytes = std::max(k_executor_receipt_bytes, outcome.receipt.size());
            projected.push_back({.call_id = outcome.call_id,
                                 .kind = ToolOutcomeKind::OUTCOME_UNKNOWN,
                                 .receipt = std::string(projected_receipt_bytes(receipt_bytes), 'r')});
            plan.calls.push_back({.call = {.id = outcome.call_id},
                                  .grant = {.receipt_bytes = receipt_bytes, .payload_bytes = 0},
                                  .immediate_outcome = std::move(outcome)});
            continue;
        }
        const auto receipt_bytes = std::max(k_executor_receipt_bytes, prepared->receipt_bytes);
        minimum_payload_bytes += prepared->minimum_payload_bytes;
        projected.push_back({.call_id = prepared->call.id,
                             .kind = ToolOutcomeKind::OUTCOME_UNKNOWN,
                             .receipt = std::string(projected_receipt_bytes(receipt_bytes), 'r')});
        plan.calls.push_back({.call = std::move(prepared->call),
                              .mode = prepared->mode,
                              .grant = {.receipt_bytes = receipt_bytes, .payload_bytes = prepared->minimum_payload_bytes},
                              .execute = std::move(prepared->execute)});
    }

    const auto capacity = payload_capacity(std::span<const ToolOutcome>(projected));
    if (!capacity || minimum_payload_bytes > k_max_tool_payload_bytes || *capacity < minimum_payload_bytes) {
        ToolBatchRejected rejected;
        rejected.outcomes.reserve(plan.calls.size());
        for (auto &call : plan.calls) {
            if (call.immediate_outcome) {
                rejected.outcomes.push_back(*std::move(call.immediate_outcome));
            } else {
                rejected.outcomes.push_back(
                    planning_failure(std::move(call.call.id), ToolOutcomeKind::NOT_STARTED, "insufficient_output_capacity"));
            }
        }
        return rejected;
    }

    auto remaining = std::min(*capacity - minimum_payload_bytes, k_max_tool_payload_bytes - minimum_payload_bytes);
    for (usize index = 0; index < plan.calls.size(); ++index) {
        const auto calls_left = plan.calls.size() - index;
        const auto share = remaining / calls_left;
        plan.calls[index].grant.payload_bytes += share;
        remaining -= share;
        if (plan.calls[index].immediate_outcome) finalize_tool_outcome(*plan.calls[index].immediate_outcome, plan.calls[index].grant);
    }
    return plan;
}

} // namespace liminal::agent::detail
