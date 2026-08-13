#pragma once

#include <array>
#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/history.h>

namespace liminal::session {

using namespace lighter::types;

struct PersistenceQueue;

struct SessionId {
    std::array<u8, 16> bytes{};
    auto operator<=>(const SessionId &) const = default;
};

SessionId generate_session_id();
Result<SessionId> parse_session_id(std::string_view text);
std::string to_string(SessionId id);

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
    INTERRUPTED,
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
    INTERRUPTED,
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
    i64 created_at_ms = 0;
};

struct SessionWorkspace {
    std::string root;
    std::string key;
};

struct SessionModelPreference {
    std::string provider;
    std::string model;
    std::optional<std::string> reasoning_effort;
};

struct ForkOrigin {
    SessionId session;
    EntryId entry;
};

struct SessionMetadata {
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
    std::optional<SessionWorkspace> workspace;
    std::string working_directory;
    std::optional<std::string> title;
    std::string preview;
    std::optional<SessionModelPreference> model_preference;
    std::optional<i64> archived_at_ms;
    std::optional<ForkOrigin> forked_from;
};

i64 unix_milliseconds_now() noexcept;

struct Session {
    Session();
    explicit Session(SessionId id);
    Session(Session &&) noexcept = default;
    Session &operator=(Session &&) noexcept = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    EntryId append(EntryPayload payload);
    TaskId start_task(std::string text);
    ProviderCallId next_provider_call();

    /// Moves the active cursor without modifying entries. Appending after
    /// selecting an ancestor creates another branch.
    Result<void> select_leaf(std::optional<EntryId> id);
    Result<void> validate() const;
    void set_model_preference(std::string provider, std::string model, std::optional<std::string> reasoning_effort);
    void attach_persistence(std::shared_ptr<PersistenceQueue> queue);

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
    SessionMetadata metadata;
    std::shared_ptr<PersistenceQueue> persistence;
};

} // namespace liminal::session
