#pragma once

#include <array>
#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
/// Applies the canonical durable first-prompt preview bound.
std::string session_preview(std::string_view text);

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
    bool operator==(const SessionWorkspace &) const = default;
};

struct SessionModelPreference {
    std::string provider;
    std::string model;
    std::optional<std::string> reasoning_effort;
};

struct ForkOrigin {
    SessionId session;
    EntryId entry;
    bool operator==(const ForkOrigin &) const = default;
};

struct SessionMetadata {
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
    std::optional<SessionWorkspace> workspace;
    std::string working_directory;
    std::optional<std::string> title;
    std::string preview;
    std::optional<SessionModelPreference> model_preference;
    std::optional<ForkOrigin> forked_from;
};

enum struct ConversationCheckpointKind {
    TASK,
    COMPACTION,
};

struct ConversationCheckpointId {
    EntryId entry;
    auto operator<=>(const ConversationCheckpointId &) const = default;
};

inline constexpr usize k_branch_leaf_example_limit = 4;

/// A safe idle boundary projected from the append-only entry tree. The leaf
/// count and bounded examples describe stable branch identities derived from
/// durable history, not separately persisted branch records.
struct ConversationCheckpoint {
    ConversationCheckpointId id;
    std::optional<ConversationCheckpointId> parent_checkpoint;
    usize depth = 0;
    ConversationCheckpointKind kind = ConversationCheckpointKind::TASK;
    std::string label;
    std::optional<TaskOutcome> task_outcome;
    bool active = false;
    bool on_active_branch = false;
    usize direct_descendants = 0;
    usize branch_leaf_count = 0;
    std::vector<ConversationCheckpointId> branch_leaf_examples;
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
    TaskId start_task(std::string text, std::optional<i64> admission_time_ms = std::nullopt);
    ProviderCallId next_provider_call();

    /// Moves the append point to a safe idle checkpoint without modifying
    /// descendants. A later append creates another branch.
    Result<void> checkout(ConversationCheckpointId checkpoint);
    /// Copies the selected semantic prefix into a new independently identified
    /// session while preserving provider-private payloads.
    Result<Session> fork_at(ConversationCheckpointId checkpoint) const;
    Result<void> validate() const;
    void set_model_preference(std::string provider, std::string model, std::optional<std::string> reasoning_effort);
    void set_title(std::optional<std::string> title);
    Result<void> attach_persistence(std::shared_ptr<PersistenceQueue> queue);
    PersistenceQueue *persistence_queue() noexcept;
    const PersistenceQueue *persistence_queue() const noexcept;

    const SessionEntry *find(EntryId id) const noexcept;
    Result<std::vector<const SessionEntry *>> branch_to(EntryId id) const;
    std::vector<const SessionEntry *> active_branch() const;
    Result<std::vector<ConversationCheckpoint>> conversation_checkpoints() const;
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

private:
    std::shared_ptr<PersistenceQueue> persistence;
};

} // namespace liminal::session
