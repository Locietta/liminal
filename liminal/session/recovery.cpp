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
        if (const auto *results = std::get_if<ToolResults>(&entry->payload); results && results->task_id == *task_id) {
            auto &call = find_call(results->provider_call_id);
            for (const auto &result : results->results) call.results.insert(result.call_id);
            continue;
        }
        if (const auto *task_finished = std::get_if<TaskFinished>(&entry->payload); task_finished && task_finished->id == *task_id) {
            finished = true;
        }
    }

    if (!task_id || finished) return recovered;
    for (auto &call : calls) {
        std::vector<provider::ToolResult> missing;
        for (const auto &tool : call.tools) {
            if (call.results.contains(tool.id)) continue;
            missing.push_back({
                .call_id = tool.id,
                .content = "Tool execution was interrupted before its result became durable. It may have partially or fully occurred; "
                           "inspect the environment before deciding whether to run it again. It will not be retried automatically.",
                .is_error = true,
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
            session.append(ToolResults{.task_id = *task_id, .provider_call_id = call.id, .results = std::move(missing)});
        }
    }
    session.append(TaskFinished{.id = *task_id, .outcome = recovered_outcome(calls)});
    recovered.recovered_tasks = 1;
    return recovered;
}

} // namespace liminal::session
