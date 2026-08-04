#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace liminal {

/// Application-level events shared by the agent controller and UI. Provider
/// wire events never cross this boundary.
struct PromptSubmitted {
    std::string text;
};

struct AssistantTextDelta {
    std::string text;
};

struct AssistantSegmentCompleted {};

struct ToolStarted {
    std::string call_id;
    std::string name;
};

struct ToolCompleted {
    std::string call_id;
    std::string name;
    bool is_error = false;
};

struct TurnCompleted {};
struct TurnCancelled {};

struct TurnFailed {
    std::string message;
};

struct SessionNotice {
    std::string text;
};

struct ModelSelected {
    std::string name;
    std::optional<std::string> effort;
};

using Event = std::variant<PromptSubmitted, AssistantTextDelta, AssistantSegmentCompleted, ToolStarted, ToolCompleted, TurnCompleted,
                           TurnCancelled, TurnFailed, SessionNotice, ModelSelected>;
using EventSink = std::copyable_function<void(const Event &) const>;

} // namespace liminal
