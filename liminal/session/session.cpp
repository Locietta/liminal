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

    std::vector<std::string> replies;
    std::string reply;
    bool in_task = false;
    const auto finish_task = [&] {
        if (!reply.empty()) replies.push_back(std::exchange(reply, {}));
    };
    for (const auto *entry : active_branch()) {
        if (std::holds_alternative<TaskStarted>(entry->payload)) {
            if (in_task) finish_task();
            in_task = true;
            continue;
        }
        if (!in_task) continue;
        const auto *output = std::get_if<OutputItemCompleted>(&entry->payload);
        if (!output) continue;
        const auto *message = std::get_if<provider::AssistantMessageItem>(&output->item);
        if (!message) continue;
        std::string segment;
        for (const auto &part : message->parts) segment += part.text;
        if (segment.empty()) continue;
        if (!reply.empty()) reply += "\n\n";
        reply += segment;
    }
    if (in_task) finish_task();
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
