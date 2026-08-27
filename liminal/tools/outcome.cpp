#include "outcome.h"

#include <string>
#include <utility>

namespace liminal {

bool tool_outcome_is_error(ToolOutcomeKind kind) noexcept { return kind != ToolOutcomeKind::SUCCEEDED; }

std::string_view tool_outcome_status(ToolOutcomeKind kind) noexcept {
    switch (kind) {
        case ToolOutcomeKind::SUCCEEDED: return "succeeded";
        case ToolOutcomeKind::FAILED: return "failed";
        case ToolOutcomeKind::NOT_STARTED: return "not_started";
        case ToolOutcomeKind::OUTCOME_UNKNOWN: return "outcome_unknown";
    }
    std::unreachable();
}

std::string render_tool_outcome(const ToolOutcome &outcome) {
    std::string result = "status: " + std::string(tool_outcome_status(outcome.kind));
    result += "\npayload_truncated: ";
    result += outcome.payload_truncated ? "true" : "false";
    if (!outcome.receipt.empty()) result += "\n" + outcome.receipt;
    if (!outcome.payload.empty()) {
        result += k_tool_outcome_payload_framing;
        result += outcome.payload;
    }
    return result;
}

} // namespace liminal
