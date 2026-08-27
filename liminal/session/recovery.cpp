#include "recovery.h"

#include <algorithm>
#include <set>
#include <utility>

namespace liminal::session {

namespace {

struct CallRecovery {
    ProviderCallId id;
    std::vector<provider::ToolCall> tools;
    std::set<std::string> results;
    std::optional<ProviderCallCompleted> completed;
    std::optional<ProviderCallAborted> aborted;
    bool settled = false;
};

TaskOutcome recovered_outcome(const std::vector<CallRecovery> &calls) {
    for (auto call = calls.rbegin(); call != calls.rend(); ++call) {
        if (call->completed) {
            switch (call->completed->loop_outcome) {
                case ProviderCallLoopOutcome::TERMINAL: return TaskOutcome::COMPLETED;
                case ProviderCallLoopOutcome::FAILED: return TaskOutcome::FAILED;
                case ProviderCallLoopOutcome::FOLLOW_UP: return TaskOutcome::INTERRUPTED;
            }
        }
        if (call->aborted) {
            switch (call->aborted->reason) {
                case ProviderCallAbortReason::CANCELLED: return TaskOutcome::CANCELLED;
                case ProviderCallAbortReason::FAILED: return TaskOutcome::FAILED;
                case ProviderCallAbortReason::INTERRUPTED: return TaskOutcome::INTERRUPTED;
            }
        }
    }
    return TaskOutcome::INTERRUPTED;
}

} // namespace

RecoveryResult recover_interrupted(Session &session) {
    RecoveryResult recovered;
    const auto branch = session.active_branch();
    std::optional<TaskId> task_id;
    bool finished = true;
    std::vector<CallRecovery> calls;
    auto find_call = [&calls](ProviderCallId id) -> CallRecovery & {
        auto found = std::ranges::find(calls, id, &CallRecovery::id);
        if (found == calls.end()) found = calls.insert(calls.end(), CallRecovery{.id = id});
        return *found;
    };

    for (const auto *entry : branch) {
        if (const auto *started = std::get_if<TaskStarted>(&entry->payload)) {
            task_id = started->id;
            finished = false;
            calls.clear();
            continue;
        }
        if (!task_id || finished) continue;
        if (const auto *output = std::get_if<OutputItemCompleted>(&entry->payload); output && output->task_id == *task_id) {
            auto &call = find_call(output->provider_call_id);
            if (const auto *tool = std::get_if<provider::ToolCallItem>(&output->item)) call.tools.push_back(tool->call);
            continue;
        }
        if (const auto *completed = std::get_if<ProviderCallCompleted>(&entry->payload); completed && completed->task_id == *task_id) {
            find_call(completed->id).completed = *completed;
            continue;
        }
        if (const auto *aborted = std::get_if<ProviderCallAborted>(&entry->payload); aborted && aborted->task_id == *task_id) {
            find_call(aborted->id).aborted = *aborted;
            continue;
        }
        if (const auto *outcomes = std::get_if<ToolOutcomes>(&entry->payload); outcomes && outcomes->task_id == *task_id) {
            auto &call = find_call(outcomes->provider_call_id);
            for (const auto &outcome : outcomes->outcomes) call.results.insert(outcome.call_id);
            continue;
        }
        if (const auto *settled = std::get_if<ProviderRoundSettled>(&entry->payload); settled && settled->task_id == *task_id) {
            find_call(settled->provider_call_id).settled = true;
            continue;
        }
        if (const auto *task_finished = std::get_if<TaskFinished>(&entry->payload); task_finished && task_finished->id == *task_id) {
            finished = true;
        }
    }

    if (!task_id || finished) return recovered;
    for (auto &call : calls) {
        std::vector<ToolOutcome> missing;
        for (const auto &tool : call.tools) {
            if (call.results.contains(tool.id)) continue;
            missing.push_back({
                .call_id = tool.id,
                .kind = ToolOutcomeKind::OUTCOME_UNKNOWN,
                .receipt = "reason: process_interrupted",
                .payload = "Tool execution was interrupted before its outcome became durable. Inspect the environment before deciding "
                           "whether to run it again; it will not be retried automatically.",
            });
        }
        if (!call.completed && !call.aborted) {
            session.append(ProviderCallAborted{
                .task_id = *task_id,
                .id = call.id,
                .reason = ProviderCallAbortReason::INTERRUPTED,
                .detail = "provider call was interrupted by process termination",
            });
            call.aborted = ProviderCallAborted{.task_id = *task_id, .id = call.id, .reason = ProviderCallAbortReason::INTERRUPTED};
        }
        if (!missing.empty()) {
            recovered.unknown_tool_outcomes += missing.size();
            session.append(ToolOutcomes{.task_id = *task_id, .provider_call_id = call.id, .outcomes = std::move(missing)});
        }
        if (!call.settled) {
            session.append(ProviderRoundSettled{
                .task_id = *task_id,
                .provider_call_id = call.id,
                .replay = call.completed && !call.aborted && call.completed->loop_outcome != ProviderCallLoopOutcome::FAILED ?
                              ProviderRoundReplay::REPLAY :
                              ProviderRoundReplay::OMIT,
            });
        }
    }
    session.append(TaskFinished{.id = *task_id, .outcome = recovered_outcome(calls)});
    recovered.recovered_tasks = 1;
    return recovered;
}

} // namespace liminal::session
