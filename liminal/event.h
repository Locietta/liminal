#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>

#include <lighter/types.hpp>

#include <liminal/provider/history.h>

namespace liminal {

using namespace lighter::types;

/// Agent-owned lifecycle generation. Provider output and tool-call identities
/// are unique only within this scope, not across the complete transcript.
struct ActivityScope {
    u64 task_generation = 0;
    u64 provider_call_generation = 0;

    friend bool operator==(const ActivityScope &, const ActivityScope &) = default;
};

/// Application-level events shared by the agent controller and UI. Provider
/// wire events never cross this boundary.
struct PromptSubmitted {
    std::string text;
};

struct AssistantTextDelta {
    std::string item_id;
    std::string text;
    ActivityScope activity_scope;
};

struct AssistantMessageCompleted {
    std::string item_id;
    std::string text;
    provider::MessagePhase phase = provider::MessagePhase::UNSPECIFIED;
    ActivityScope activity_scope;
};

struct ToolStarted {
    std::string call_id;
    std::string name;
    std::string description;
    std::string command;
    ActivityScope activity_scope;
};

struct ToolCompleted {
    std::string call_id;
    std::string name;
    std::string description;
    std::string command;
    std::string summary;
    bool is_error = false;
    ActivityScope activity_scope;
};

struct ProviderActivityCompleted {
    ActivityScope activity_scope;
};

struct TaskCompleted {
    u64 task_generation = 0;
};
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

using Event = std::variant<PromptSubmitted, AssistantTextDelta, AssistantMessageCompleted, ToolStarted, ToolCompleted,
                           ProviderActivityCompleted, TaskCompleted, TaskCancelled, TaskFailed, SessionNotice, ModelSelected>;
using EventSink = std::copyable_function<void(const Event &) const>;

} // namespace liminal
