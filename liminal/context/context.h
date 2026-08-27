#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/history.h>
#include <liminal/session/session.h>

namespace liminal::context {

using namespace lighter::types;

/// Liminal-owned instruction precedence. Provider adapters lower these
/// semantic authorities to the closest wire-protocol role.
enum struct InstructionAuthority {
    RUNTIME,
    APPLICATION,
    PROJECT,
};

/// The principal that made an instruction authoritative. Trust describes why
/// a source may govern the agent; authority describes its precedence.
enum struct InstructionTrust {
    PLATFORM,
    OPERATOR,
    WORKSPACE,
};

struct InstructionSource {
    InstructionAuthority authority = InstructionAuthority::APPLICATION;
    InstructionTrust trust = InstructionTrust::PLATFORM;
    std::string origin;
    /// Directory subtree where this instruction applies. An absent scope is
    /// global to the agent instance.
    std::optional<std::filesystem::path> scope;
    std::string content;
};

struct ContextBudget {
    std::optional<u32> context_window_tokens;
    u32 reserved_output_tokens = 0;
    u32 safety_margin_tokens = 0;
};

struct ContextUsage {
    usize instruction_bytes = 0;
    usize conversation_bytes = 0;
    usize tool_bytes = 0;
    usize opaque_bytes = 0;
    usize estimated_input_tokens = 0;
    std::optional<usize> reported_context_tokens;
    usize estimated_trailing_tokens = 0;
    std::optional<usize> input_budget_tokens;
    std::optional<i64> remaining_input_tokens;
};

/// Inspectable description of the context selected for one provider call.
/// `provider_history` is derived request state, not the agent's source of
/// truth.
struct ContextManifest {
    std::vector<InstructionSource> instructions;
    std::vector<InstructionSource> omitted_duplicates;
    session::SessionId session_id;
    std::vector<session::EntryId> session_entries;
    usize omitted_session_entries = 0;
    usize omitted_checkpoint_entries = 0;
    usize omitted_budget_entries = 0;
    std::optional<session::EntryId> active_checkpoint;
    ContextBudget budget;
    ContextUsage usage;
    provider::History provider_history;
};

/// Formats manifest metadata without exposing instruction or tool contents.
std::string describe(const ContextManifest &manifest);

struct ContextBuilder {
    /// Optional projected outcomes settle the latest provisional provider
    /// round for admission planning and participate in exact usage estimation.
    Result<ContextManifest> build(std::span<const InstructionSource> sources, const session::Session &session, ContextBudget budget = {},
                                  std::span<const ToolOutcome> projected_tool_outcomes = {}) const;

    /// Converts a provider compaction result into a semantic checkpoint after
    /// verifying that its resolved instruction prefix is unchanged.
    Result<session::ContextCheckpoint> take_checkpoint(provider::History history, const ContextManifest &manifest) const;
};

} // namespace liminal::context
