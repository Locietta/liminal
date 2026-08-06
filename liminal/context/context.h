#pragma once

#include <span>
#include <string>
#include <vector>

#include "liminal/error.h"
#include "liminal/provider/history.h"

namespace liminal::context {

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
    provider::History provider_history;
};

struct ContextBuilder {
    Result<ContextManifest> build(std::span<const InstructionSource> sources, const provider::History &conversation) const;

    /// Removes the resolved instruction prefix after a provider operation.
    /// This also verifies that providers did not move instructions into the
    /// mutable conversation.
    Result<provider::History> take_conversation(provider::History history, const ContextManifest &manifest) const;
};

} // namespace liminal::context
