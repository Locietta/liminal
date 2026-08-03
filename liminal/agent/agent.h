#pragma once

#include <string>
#include <string_view>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/event.h"
#include "liminal/model/model.h"
#include "liminal/tools/tools.h"

namespace liminal {

struct Agent {
    Agent(model::Choice model, ToolSet &tools) : model(std::move(model)), tools(&tools) {}

    /// One transactional user turn. Partial UI output is emitted as typed
    /// events while provider history commits only after a terminal response.
    lighter::Task<void, Error> run_turn(std::string prompt, const EventSink &events);

    lighter::Task<void, Error> compact(std::string_view instructions);

    void select_model(model::Choice next) { model = std::move(next); }

    model::Choice model;
    ToolSet *tools;
    provider::History history;
};

} // namespace liminal
