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

struct UserMessage {
    std::string text;
};

struct AgentOutput {
    std::vector<provider::Part> parts;
    provider::Usage usage;
    std::string model;
    std::string request_id;
};

/// All tool results from one provider round. Context policies must retain this
/// entry together with the preceding AgentOutput that issued the calls.
struct ToolResults {
    std::vector<provider::ToolResult> results;
};

/// Synthetic, untrusted input produced by compaction. It lowers to a user
/// message but never acquires instruction authority.
struct ContextInput {
    std::vector<provider::Part> parts;
};

using CheckpointItem = std::variant<ContextInput, AgentOutput>;

/// A derived context replacement. Earlier entries remain in the append-only
/// log, but context projection starts here and expands these items.
struct ContextCheckpoint {
    std::vector<CheckpointItem> items;
};

using EntryPayload = std::variant<UserMessage, AgentOutput, ToolResults, ContextCheckpoint>;

struct SessionEntry {
    EntryId id;
    std::optional<EntryId> parent_id;
    EntryPayload payload;
};

struct Session {
    Session();
    explicit Session(SessionId id);

    EntryId append(EntryPayload payload);

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
};

} // namespace liminal::session
