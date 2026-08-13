#pragma once

#include <string>
#include <string_view>
#include <optional>
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
    Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions, session::Session session);

    /// Runs one user task. Semantic session entries are appended as their
    /// lifecycle boundaries complete, so partial progress remains resumable.
    lighter::Task<void, Error, lighter::Cancellation> run_task(std::string prompt, EventSink events,
                                                               std::optional<lighter::CancellationToken> cancellation = std::nullopt);

    lighter::Task<void, Error> compact(std::string_view instructions);

    Result<context::ContextManifest> context_manifest() const;

    void select_model(model::Choice next);

    model::Choice model;
    ToolSet *tools;
    std::vector<context::InstructionSource> instructions;
    session::Session session;

private:
    lighter::Task<bool, Error, lighter::Cancellation> run_task_loop(session::TaskId task_id, const EventSink &events,
                                                                    const std::optional<lighter::CancellationToken> &cancellation);
};

} // namespace liminal
