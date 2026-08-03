#pragma once

#include <string>
#include <string_view>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/event.h"
#include "liminal/provider/provider.h"
#include "liminal/tools/tools.h"

namespace liminal {

/// A provider selected and configured for use: the facade proxy plus display
/// metadata owned by the application layer.
struct ProviderChoice {
    provider::Provider handle;
    std::string name;
    std::string model;
};

struct Agent {
    Agent(ProviderChoice provider, ToolSet &tools) : provider(std::move(provider)), tools(&tools) {}

    /// One transactional user turn. Partial UI output is emitted as typed
    /// events while provider history commits only after a terminal response.
    lighter::Task<void, Error> run_turn(std::string prompt, const EventSink &events);

    lighter::Task<void, Error> compact(std::string_view instructions);

    void switch_provider(ProviderChoice next) { provider = std::move(next); }

    ProviderChoice provider;
    ToolSet *tools;
    provider::History history;
};

} // namespace liminal
