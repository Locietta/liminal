#pragma once

#include <string>
#include <string_view>

namespace liminal {

enum struct ToolOutcomeKind {
    SUCCEEDED,
    FAILED,
    NOT_STARTED,
    OUTCOME_UNKNOWN,
};

/// Durable, provider-independent result of one local tool call. `receipt`
/// contains mandatory control data; `payload` is optional, grant-bounded data.
struct ToolOutcome {
    std::string call_id;
    ToolOutcomeKind kind = ToolOutcomeKind::SUCCEEDED;
    std::string receipt;
    std::string payload;
    bool payload_truncated = false;
};

inline constexpr std::string_view k_tool_outcome_payload_framing = "\n\noutput:\n";

bool tool_outcome_is_error(ToolOutcomeKind kind) noexcept;
std::string_view tool_outcome_status(ToolOutcomeKind kind) noexcept;
std::string render_tool_outcome(const ToolOutcome &outcome);

} // namespace liminal
