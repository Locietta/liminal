#include "session.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session {

namespace {

std::atomic<u64> next_session_id = 1;

} // namespace

Session::Session() : Session(SessionId{.value = next_session_id.fetch_add(1, std::memory_order_relaxed)}) {}

Session::Session(SessionId id) : id(id) {}

EntryId Session::append(EntryPayload payload) {
    const EntryId entry_id{.value = next_entry_id++};
    entries.push_back({
        .id = entry_id,
        .parent_id = active_leaf,
        .payload = std::move(payload),
    });
    active_leaf = entry_id;
    return entry_id;
}

TaskId Session::start_task(std::string text) {
    const TaskId task_id{.value = next_task_id++};
    append(TaskStarted{.id = task_id, .text = std::move(text)});
    return task_id;
}

ProviderCallId Session::next_provider_call() { return {.value = next_provider_call_id++}; }

Result<void> Session::select_leaf(std::optional<EntryId> id) {
    if (id && !find(*id)) {
        return lighter::outcome_error(Error::protocol("cannot select an unknown session entry"));
    }
    active_leaf = id;
    return {};
}

const SessionEntry *Session::find(EntryId id) const noexcept {
    if (id.value == 0 || id.value > entries.size()) {
        return nullptr;
    }
    const auto &entry = entries[id.value - 1];
    return entry.id == id ? &entry : nullptr;
}

std::vector<const SessionEntry *> Session::active_branch() const {
    std::vector<const SessionEntry *> branch;
    auto cursor = active_leaf;
    while (cursor) {
        const auto *entry = find(*cursor);
        if (!entry) {
            break;
        }
        branch.push_back(entry);
        cursor = entry->parent_id;
    }
    std::ranges::reverse(branch);
    return branch;
}

std::optional<std::string> Session::reply_from_latest(usize ordinal) const {
    if (ordinal == 0) return std::nullopt;

    struct ReplyCandidate {
        ProviderCallId call_id;
        std::string explicit_final;
        std::string last_unphased;
    };

    std::vector<std::string> replies;
    std::optional<TaskId> active_task;
    std::optional<ProviderCallId> terminal_call;
    std::vector<ReplyCandidate> candidates;
    for (const auto *entry : active_branch()) {
        if (const auto *started = std::get_if<TaskStarted>(&entry->payload)) {
            active_task = started->id;
            terminal_call.reset();
            candidates.clear();
            continue;
        }
        if (!active_task) continue;
        const auto *output = std::get_if<OutputItemCompleted>(&entry->payload);
        if (output && output->task_id == *active_task) {
            const auto *message = std::get_if<provider::AssistantMessageItem>(&output->item);
            if (!message || message->phase == provider::MessagePhase::COMMENTARY) continue;
            std::string text;
            for (const auto &part : message->parts) text += part.text;
            if (text.empty()) continue;
            auto candidate = std::ranges::find(candidates, output->provider_call_id, &ReplyCandidate::call_id);
            if (candidate == candidates.end()) {
                candidate = candidates.insert(candidates.end(), ReplyCandidate{.call_id = output->provider_call_id});
            }
            if (message->phase == provider::MessagePhase::FINAL) {
                if (!candidate->explicit_final.empty()) candidate->explicit_final += "\n\n";
                candidate->explicit_final += text;
            } else {
                candidate->last_unphased = std::move(text);
            }
            continue;
        }
        const auto *call = std::get_if<ProviderCallCompleted>(&entry->payload);
        if (call && call->task_id == *active_task && call->loop_outcome == ProviderCallLoopOutcome::TERMINAL) {
            terminal_call = call->id;
            continue;
        }
        const auto *finished = std::get_if<TaskFinished>(&entry->payload);
        if (!finished || finished->id != *active_task) continue;
        if (finished->outcome == TaskOutcome::COMPLETED && terminal_call) {
            const auto candidate = std::ranges::find(candidates, *terminal_call, &ReplyCandidate::call_id);
            if (candidate == candidates.end()) {
                active_task.reset();
                continue;
            }
            auto reply = !candidate->explicit_final.empty() ? std::move(candidate->explicit_final) : std::move(candidate->last_unphased);
            if (!reply.empty()) replies.push_back(std::move(reply));
        }
        active_task.reset();
    }
    if (ordinal > replies.size()) return std::nullopt;
    return replies[replies.size() - ordinal];
}

u64 Session::tokens_used() const noexcept {
    u64 total = 0;
    for (const auto &entry : entries) {
        const auto *call = std::get_if<ProviderCallCompleted>(&entry.payload);
        if (!call) continue;
        const auto &usage = call->completion.usage;
        const auto uncached_tokens = usage.output_tokens > std::numeric_limits<u64>::max() - usage.input_tokens ?
                                         std::numeric_limits<u64>::max() :
                                         usage.input_tokens + usage.output_tokens;
        const auto response_tokens = usage.context_tokens != 0 ? usage.context_tokens : uncached_tokens;
        total = response_tokens > std::numeric_limits<u64>::max() - total ? std::numeric_limits<u64>::max() : total + response_tokens;
    }
    return total;
}

} // namespace liminal::session
