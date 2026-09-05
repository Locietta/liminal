#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/context/context.h>
#include <liminal/error.h>
#include <liminal/event.h>
#include <liminal/model/model.h>
#include <liminal/session/session.h>
#include <liminal/tools/tools.h>

namespace liminal {

struct TaskTarget {
    session::SessionId session_id;
    session::TaskId task_id;
    auto operator<=>(const TaskTarget &) const = default;
};

struct Agent {
    Agent(model::Choice model, ToolSet &tools);
    Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions);
    Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions, session::Session session);

    /// Runs one user task. Semantic session entries are appended as their
    /// lifecycle boundaries complete, so partial progress remains resumable.
    lighter::Task<void, Error, lighter::Cancellation> run_task(std::string prompt, EventSink events,
                                                               std::optional<lighter::CancellationToken> cancellation = std::nullopt);

    lighter::Task<void, Error> compact(std::string instructions);

    /// Called on the agent's event loop. A stale task ID is rejected rather
    /// than redirecting a correction into a later task.
    Result<session::EntryId> steer(TaskTarget expected_task, std::string text);
    std::optional<TaskTarget> active_task() const noexcept {
        if (!current_task) return std::nullopt;
        return TaskTarget{.session_id = session.id, .task_id = *current_task};
    }

    Result<context::ContextManifest> context_manifest() const;

    void select_model(model::Choice next);
    void replace_session(model::Choice next_model, session::Session next_session);

    model::Choice model;
    ToolSet *tools;
    std::vector<context::InstructionSource> instructions;
    session::Session session;

private:
    lighter::Task<bool, Error, lighter::Cancellation> run_task_loop(session::TaskId task_id, u64 activity_task_generation,
                                                                    std::string_view prompt, const EventSink &events,
                                                                    const std::optional<lighter::CancellationToken> &cancellation);

    /// Replaces the active context with an automatic compaction checkpoint
    /// followed by unprocessed input, verifying that the next request fits
    /// and compaction reduces context before committing the checkpoint.
    lighter::Task<void, Error, lighter::Cancellation> compact_automatically(std::span<const std::string> admitted_inputs,
                                                                            std::span<const std::string> queued_inputs,
                                                                            const std::optional<lighter::CancellationToken> &cancellation);

    std::vector<session::PendingSteering> current_steering() const;
    std::optional<session::TaskId> current_task;

    u64 next_activity_task_generation = 1;
    u64 next_activity_provider_call_generation = 1;
};

} // namespace liminal
