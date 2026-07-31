#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/provider/provider.h"
#include "liminal/tools/tools.h"

namespace liminal {

/// A provider selected and configured for use: the facade proxy plus the
/// display metadata the REPL shows. Produced by the ProviderFactory.
struct ProviderChoice {
    provider::Provider handle;
    std::string name;  ///< e.g. "anthropic", "openai"
    std::string model; ///< resolved model id, for display
};

/// Builds a configured provider by name ("anthropic" / "openai"). Supplied by
/// main so credential/env policy stays out of the agent layer; the REPL uses
/// it for /switch.
using ProviderFactory = std::function<Result<ProviderChoice>(std::string_view name)>;

struct Agent {
    Agent(ProviderChoice provider, ToolSet &tools) : provider(std::move(provider)), tools(&tools) {}

    /// One user turn: send prompt, stream the response (through `callbacks`),
    /// execute tool calls until the model stops. History is transactional -
    /// a failed or cancelled turn leaves it untouched.
    lighter::Task<void, Error> run_turn(std::string prompt, const provider::StreamCallbacks &callbacks);

    /// Compact the transcript in place (provider-native fast path or local
    /// summarization - the provider decides).
    lighter::Task<void, Error> compact(std::string_view instructions);

    /// Swap the provider, keeping history. Foreign opaque parts (private
    /// provider state) silently drop on the next request; visible
    /// conversation carries over. Only sound between turns.
    void switch_provider(ProviderChoice next) { provider = std::move(next); }

    ProviderChoice provider;
    ToolSet *tools;
    provider::History history;
};

/// Interactive stdin/stdout REPL around an Agent. Returns the process exit
/// code. `interrupts` powers Ctrl-C handling: first cancels the in-flight
/// turn (or /compact), second hard-exits.
lighter::Task<i32> run_repl(Agent &agent, lighter::InterruptSource &interrupts, const ProviderFactory &factory);

} // namespace liminal
