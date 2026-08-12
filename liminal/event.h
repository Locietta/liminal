#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>

#include <liminal/provider/history.h>

namespace liminal {

/// Application-level events shared by the agent controller and UI. Provider
/// wire events never cross this boundary.
struct PromptSubmitted {
    std::string text;
};

struct AssistantTextDelta {
    std::string item_id;
    std::string text;
};

struct AssistantMessageCompleted {
    std::string item_id;
    provider::MessagePhase phase = provider::MessagePhase::UNSPECIFIED;
};

struct ToolStarted {
    std::string call_id;
    std::string name;
    std::string description;
    std::string command;
};

struct ToolCompleted {
    std::string call_id;
    std::string name;
    std::string description;
    std::string command;
    std::string summary;
    bool is_error = false;
};

struct TaskCompleted {};
struct TaskCancelled {};

struct TaskFailed {
    std::string message;
};

struct SessionNotice {
    std::string text;
};

struct ModelSelected {
    std::string name;
    std::optional<std::string> effort;
};

using Event = std::variant<PromptSubmitted, AssistantTextDelta, AssistantMessageCompleted, ToolStarted, ToolCompleted, TaskCompleted,
                           TaskCancelled, TaskFailed, SessionNotice, ModelSelected>;
using EventSink = std::copyable_function<void(const Event &) const>;

} // namespace liminal
