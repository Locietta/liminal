#pragma once

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

struct InstructionSource {
    InstructionAuthority authority = InstructionAuthority::APPLICATION;
    std::string origin;
    std::string content;
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
    provider::History provider_history;
};

struct ContextBuilder {
    Result<ContextManifest> build(std::span<const InstructionSource> sources, const session::Session &session) const;

    /// Converts a provider compaction result into a semantic checkpoint after
    /// verifying that its resolved instruction prefix is unchanged.
    Result<session::ContextCheckpoint> take_checkpoint(provider::History history, const ContextManifest &manifest) const;
};

} // namespace liminal::context
