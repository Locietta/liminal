#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/context/context.h>
#include <liminal/error.h>
#include <liminal/event.h>
#include <liminal/model/model.h>
#include <liminal/session/session.h>
#include <liminal/tools/tools.h>

namespace liminal {

struct Agent {
    Agent(model::Choice model, ToolSet &tools);
    Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions);

    /// One transactional user turn. Partial UI output is emitted as typed
    /// events while provider history commits only after a terminal response.
    lighter::Task<void, Error> run_turn(std::string prompt, EventSink events);

    lighter::Task<void, Error> compact(std::string_view instructions);

    Result<context::ContextManifest> context_manifest() const;

    void select_model(model::Choice next) { model = std::move(next); }

    model::Choice model;
    ToolSet *tools;
    std::vector<context::InstructionSource> instructions;
    session::Session session;
};

} // namespace liminal
