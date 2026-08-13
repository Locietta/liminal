#pragma once

#include <compare>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/history.h>

namespace liminal::session {

using namespace lighter::types;

struct SessionId {
    u64 value = 0;
    auto operator<=>(const SessionId &) const = default;
};

struct EntryId {
    u64 value = 0;
    auto operator<=>(const EntryId &) const = default;
};

struct TaskId {
    u64 value = 0;
    auto operator<=>(const TaskId &) const = default;
};

struct ProviderCallId {
    u64 value = 0;
    auto operator<=>(const ProviderCallId &) const = default;
};

struct TaskStarted {
    TaskId id;
    std::string text;
};

struct OutputItemCompleted {
    TaskId task_id;
    ProviderCallId provider_call_id;
    provider::OutputItem item;
};

enum struct ProviderCallLoopOutcome {
    FOLLOW_UP,
    TERMINAL,
    FAILED,
};

struct ProviderCallCompleted {
    TaskId task_id;
    ProviderCallId id;
    provider::ProviderCallCompletion completion;
    ProviderCallLoopOutcome loop_outcome = ProviderCallLoopOutcome::FAILED;
};

enum struct ProviderCallAbortReason {
    CANCELLED,
    FAILED,
};

struct ProviderCallAborted {
    TaskId task_id;
    ProviderCallId id;
    ProviderCallAbortReason reason = ProviderCallAbortReason::FAILED;
    std::string detail;
};

struct ToolResults {
    TaskId task_id;
    ProviderCallId provider_call_id;
    std::vector<provider::ToolResult> results;
};

enum struct TaskOutcome {
    COMPLETED,
    CANCELLED,
    FAILED,
};

struct TaskFinished {
    TaskId id;
    TaskOutcome outcome = TaskOutcome::COMPLETED;
};

/// Synthetic, untrusted input produced by compaction. It lowers to a user
/// message but never acquires instruction authority.
struct ContextInput {
    std::vector<provider::Part> parts;
};

struct CheckpointOutput {
    provider::OutputItem item;
};

using CheckpointItem = std::variant<ContextInput, CheckpointOutput>;

/// A derived context replacement. Earlier entries remain in the append-only
/// log, but context projection starts here and expands these items.
struct ContextCheckpoint {
    std::vector<CheckpointItem> items;
};

using EntryPayload = std::variant<TaskStarted, OutputItemCompleted, ProviderCallCompleted, ProviderCallAborted, ToolResults, TaskFinished,
                                  ContextCheckpoint>;

struct SessionEntry {
    EntryId id;
    std::optional<EntryId> parent_id;
    EntryPayload payload;
};

struct Session {
    Session();
    explicit Session(SessionId id);

    EntryId append(EntryPayload payload);
    TaskId start_task(std::string text);
    ProviderCallId next_provider_call();

    /// Moves the active cursor without modifying entries. Appending after
    /// selecting an ancestor creates another branch.
    Result<void> select_leaf(std::optional<EntryId> id);

    const SessionEntry *find(EntryId id) const noexcept;
    std::vector<const SessionEntry *> active_branch() const;
    /// Returns the Nth-newest textual assistant reply on the active branch.
    std::optional<std::string> reply_from_latest(usize ordinal = 1) const;
    u64 tokens_used() const noexcept;

    SessionId id;
    std::vector<SessionEntry> entries;
    std::optional<EntryId> active_leaf;
    u64 next_entry_id = 1;
    u64 next_task_id = 1;
    u64 next_provider_call_id = 1;
};

} // namespace liminal::session
