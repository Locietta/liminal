#include "session.h"

#include "persistence.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/time_generator_v7.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <lighter/async/vocab/outcome.h>
#include <lighter/encoding/utf8.h>
#include <lighter/utils/panic.h>

#include <liminal/text.h>

namespace liminal::session {

SessionId generate_session_id() {
    const auto uuid = boost::uuids::time_generator_v7{}();
    SessionId id;
    std::ranges::copy(uuid, id.bytes.begin());
    return id;
}

Result<SessionId> parse_session_id(std::string_view text) {
    try {
        const auto uuid = boost::uuids::string_generator{}(text.begin(), text.end());
        SessionId id;
        std::ranges::copy(uuid, id.bytes.begin());
        return id;
    } catch (const std::exception &) {
        return lighter::outcome_error(Error::config("invalid session id: '" + std::string(text) + "'"));
    }
}

std::string to_string(SessionId id) {
    boost::uuids::uuid uuid;
    std::ranges::copy(id.bytes, uuid.begin());
    return boost::uuids::to_string(uuid);
}

i64 unix_milliseconds_now() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {

i64 mutation_timestamp(const SessionMetadata &metadata) noexcept {
    return std::max({unix_milliseconds_now(), metadata.created_at_ms, metadata.updated_at_ms});
}

} // namespace

Session::Session() : Session(generate_session_id()) {}

Session::Session(SessionId id) : id(id) {
    metadata.created_at_ms = unix_milliseconds_now();
    metadata.updated_at_ms = metadata.created_at_ms;
}

EntryId Session::append(EntryPayload payload) {
    const EntryId entry_id{.value = next_entry_id++};
    const auto created_at = mutation_timestamp(metadata);
    entries.push_back({
        .id = entry_id,
        .parent_id = active_leaf,
        .payload = std::move(payload),
        .created_at_ms = created_at,
    });
    active_leaf = entry_id;
    if (persistence) persistence->enqueue(make_delta(*this, std::span(&entries.back(), 1)));
    return entry_id;
}

TaskId Session::start_task(std::string text, std::optional<i64> admission_time_ms) {
    const TaskId task_id{.value = next_task_id++};
    metadata.updated_at_ms = std::max(metadata.updated_at_ms, admission_time_ms.value_or(unix_milliseconds_now()));
    if (metadata.preview.empty()) {
        constexpr usize k_preview_limit = 240;
        metadata.preview = bounded_utf8(text, k_preview_limit);
    }
    append(TaskStarted{.id = task_id, .text = std::move(text)});
    return task_id;
}

ProviderCallId Session::next_provider_call() { return {.value = next_provider_call_id++}; }

Result<void> Session::checkout(ConversationCheckpointId checkpoint) {
    auto projected = conversation_checkpoints();
    if (!projected) return lighter::outcome_error(std::move(projected).error());
    if (std::ranges::none_of(*projected, [checkpoint](const ConversationCheckpoint &item) { return item.id == checkpoint; })) {
        return lighter::outcome_error(Error::protocol("requested conversation checkpoint is missing or unsafe"));
    }
    active_leaf = checkpoint.entry;
    if (persistence) persistence->enqueue(make_delta(*this, {}));
    return {};
}

void Session::set_model_preference(std::string provider, std::string model, std::optional<std::string> reasoning_effort) {
    metadata.model_preference = SessionModelPreference{
        .provider = std::move(provider),
        .model = std::move(model),
        .reasoning_effort = std::move(reasoning_effort),
    };
    if (persistence && !entries.empty()) persistence->enqueue(make_delta(*this, {}));
}

void Session::set_title(std::optional<std::string> title) {
    if (title && title->empty()) title.reset();
    contract_assert(!title || (title->size() <= 200 && lighter::encoding::utf8::is_valid(*title)));
    metadata.title = std::move(title);
    if (persistence && !entries.empty()) persistence->enqueue(make_delta(*this, {}));
}

Result<void> Session::attach_persistence(std::shared_ptr<PersistenceQueue> queue) {
    if (!queue) return lighter::outcome_error(Error::storage("cannot attach an empty persistence queue"));
    if (queue->session_id() != id) {
        return lighter::outcome_error(Error::storage("persistence queue belongs to another session"));
    }
    if (persistence) return lighter::outcome_error(Error::storage("session persistence is already attached"));
    persistence = std::move(queue);
    return {};
}

PersistenceQueue *Session::persistence_queue() noexcept { return persistence.get(); }

const PersistenceQueue *Session::persistence_queue() const noexcept { return persistence.get(); }

Result<void> Session::validate() const {
    if (next_entry_id != entries.size() + 1) {
        return lighter::outcome_error(Error::protocol("session next entry id does not follow the append-only entry sequence"));
    }
    u64 maximum_task_id = 0;
    u64 maximum_provider_call_id = 0;
    for (usize index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        if (entry.id.value != index + 1) {
            return lighter::outcome_error(Error::protocol("session entries are not dense and ordered"));
        }
        if (entry.parent_id && (entry.parent_id->value == 0 || entry.parent_id->value >= entry.id.value)) {
            return lighter::outcome_error(Error::protocol("session entry parent does not precede its child"));
        }
        if (entry.created_at_ms <= 0) {
            return lighter::outcome_error(Error::protocol("session entry has an invalid creation timestamp"));
        }
        std::visit(
            [&maximum_task_id, &maximum_provider_call_id](const auto &payload) {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<T, TaskStarted> || std::same_as<T, TaskFinished>) {
                    maximum_task_id = std::max(maximum_task_id, payload.id.value);
                } else if constexpr (std::same_as<T, OutputItemCompleted> || std::same_as<T, ToolResults>) {
                    maximum_task_id = std::max(maximum_task_id, payload.task_id.value);
                    maximum_provider_call_id = std::max(maximum_provider_call_id, payload.provider_call_id.value);
                } else if constexpr (std::same_as<T, ProviderCallCompleted> || std::same_as<T, ProviderCallAborted>) {
                    maximum_task_id = std::max(maximum_task_id, payload.task_id.value);
                    maximum_provider_call_id = std::max(maximum_provider_call_id, payload.id.value);
                }
            },
            entry.payload);
    }
    if (active_leaf && !find(*active_leaf)) {
        return lighter::outcome_error(Error::protocol("session active leaf does not identify an entry"));
    }
    if (next_task_id <= maximum_task_id || next_provider_call_id <= maximum_provider_call_id) {
        return lighter::outcome_error(Error::protocol("session lifecycle counters do not follow their durable identifiers"));
    }
    if (metadata.created_at_ms <= 0 || metadata.updated_at_ms < metadata.created_at_ms) {
        return lighter::outcome_error(Error::protocol("session catalog timestamps are invalid"));
    }
    if (metadata.preview.size() > 240 || !lighter::encoding::utf8::is_valid(metadata.preview)) {
        return lighter::outcome_error(Error::protocol("session preview is not valid bounded UTF-8"));
    }
    if (metadata.title &&
        (metadata.title->empty() || metadata.title->size() > 200 || !lighter::encoding::utf8::is_valid(*metadata.title))) {
        return lighter::outcome_error(Error::protocol("session title is not valid bounded UTF-8"));
    }
    if (metadata.workspace && (metadata.workspace->root.empty() || metadata.workspace->key.empty())) {
        return lighter::outcome_error(Error::protocol("session workspace metadata is incomplete"));
    }
    if (metadata.model_preference && (metadata.model_preference->provider.empty() || metadata.model_preference->model.empty())) {
        return lighter::outcome_error(Error::protocol("session model preference is incomplete"));
    }
    if (metadata.forked_from && metadata.forked_from->entry.value == 0) {
        return lighter::outcome_error(Error::protocol("session fork origin has an invalid entry"));
    }
    return {};
}

const SessionEntry *Session::find(EntryId id) const noexcept {
    if (id.value == 0 || id.value > entries.size()) {
        return nullptr;
    }
    const auto &entry = entries[id.value - 1];
    return entry.id == id ? &entry : nullptr;
}

Result<std::vector<const SessionEntry *>> Session::branch_to(EntryId id) const {
    if (!find(id)) return lighter::outcome_error(Error::protocol("requested session entry was not found"));
    std::vector<const SessionEntry *> branch;
    std::optional<EntryId> cursor = id;
    while (cursor) {
        const auto *entry = find(*cursor);
        if (!entry) return lighter::outcome_error(Error::protocol("session entry ancestry is incomplete"));
        branch.push_back(entry);
        cursor = entry->parent_id;
    }
    std::ranges::reverse(branch);
    return branch;
}

std::vector<const SessionEntry *> Session::active_branch() const {
    if (!active_leaf) return {};
    auto branch = branch_to(*active_leaf);
    lighter::check(branch.has_value(), "active session branch is invalid");
    return *std::move(branch);
}

Result<std::vector<ConversationCheckpoint>> Session::conversation_checkpoints() const {
    struct PathState {
        std::optional<TaskId> active_task;
        std::string_view task_label;
        std::optional<ConversationCheckpointId> checkpoint;
        usize checkpoint_depth = 0;
    };

    std::vector<bool> active_ancestry(entries.size() + 1);
    for (const auto *entry : active_branch()) active_ancestry[entry->id.value] = true;

    std::vector<PathState> states(entries.size() + 1);
    std::vector<ConversationCheckpoint> checkpoints;
    std::vector<usize> checkpoint_indices(entries.size() + 1, std::numeric_limits<usize>::max());
    u64 expected_entry_id = 1;
    for (const auto &entry : entries) {
        if (entry.id.value != expected_entry_id++) {
            return lighter::outcome_error(Error::protocol("conversation history has an invalid entry identity"));
        }
        PathState state;
        if (entry.parent_id) {
            if (entry.parent_id->value == 0 || entry.parent_id->value >= entry.id.value) {
                return lighter::outcome_error(Error::protocol("conversation history has an invalid parent"));
            }
            state = states[entry.parent_id->value];
        }

        std::optional<ConversationCheckpoint> checkpoint;
        if (const auto *started = std::get_if<TaskStarted>(&entry.payload)) {
            if (state.active_task) {
                return lighter::outcome_error(Error::protocol("conversation branch starts a task before the previous task finished"));
            }
            state.active_task = started->id;
            state.task_label = started->text;
        } else if (const auto *output = std::get_if<OutputItemCompleted>(&entry.payload)) {
            if (!state.active_task || *state.active_task != output->task_id) {
                return lighter::outcome_error(Error::protocol("conversation branch has provider output outside its active task"));
            }
        } else if (const auto *completed = std::get_if<ProviderCallCompleted>(&entry.payload)) {
            if (!state.active_task || *state.active_task != completed->task_id) {
                return lighter::outcome_error(Error::protocol("conversation branch completes a provider call outside its active task"));
            }
        } else if (const auto *aborted = std::get_if<ProviderCallAborted>(&entry.payload)) {
            if (!state.active_task || *state.active_task != aborted->task_id) {
                return lighter::outcome_error(Error::protocol("conversation branch aborts a provider call outside its active task"));
            }
        } else if (const auto *results = std::get_if<ToolResults>(&entry.payload)) {
            if (!state.active_task || *state.active_task != results->task_id) {
                return lighter::outcome_error(Error::protocol("conversation branch has tool results outside its active task"));
            }
        } else if (const auto *finished = std::get_if<TaskFinished>(&entry.payload)) {
            if (!state.active_task || *state.active_task != finished->id) {
                return lighter::outcome_error(Error::protocol("conversation branch finishes a task that is not active"));
            }
            checkpoint = ConversationCheckpoint{
                .id = ConversationCheckpointId{entry.id},
                .parent_checkpoint = state.checkpoint,
                .depth = state.checkpoint ? state.checkpoint_depth + 1 : 0,
                .kind = ConversationCheckpointKind::TASK,
                .label = std::string(state.task_label),
                .task_outcome = finished->outcome,
                .active = active_leaf == entry.id,
                .on_active_branch = active_ancestry[entry.id.value],
            };
            state.active_task.reset();
            state.task_label = {};
        } else if (std::holds_alternative<ContextCheckpoint>(entry.payload) && !state.active_task) {
            checkpoint = ConversationCheckpoint{
                .id = ConversationCheckpointId{entry.id},
                .parent_checkpoint = state.checkpoint,
                .depth = state.checkpoint ? state.checkpoint_depth + 1 : 0,
                .kind = ConversationCheckpointKind::COMPACTION,
                .label = "History compacted",
                .active = active_leaf == entry.id,
                .on_active_branch = active_ancestry[entry.id.value],
            };
        }

        if (checkpoint) {
            state.checkpoint = checkpoint->id;
            state.checkpoint_depth = checkpoint->depth;
            checkpoint_indices[entry.id.value] = checkpoints.size();
            checkpoints.push_back(*std::move(checkpoint));
        }
        states[entry.id.value] = std::move(state);
    }

    std::vector<std::vector<usize>> children(checkpoints.size());
    for (usize index = 0; index < checkpoints.size(); ++index) {
        const auto parent = checkpoints[index].parent_checkpoint;
        if (!parent) continue;
        const auto parent_index = checkpoint_indices[parent->entry.value];
        if (parent_index == std::numeric_limits<usize>::max()) {
            return lighter::outcome_error(Error::protocol("conversation checkpoint ancestry is incomplete"));
        }
        children[parent_index].push_back(index);
        ++checkpoints[parent_index].direct_descendants;
    }
    for (usize index = checkpoints.size(); index-- > 0;) {
        if (children[index].empty()) {
            checkpoints[index].branch_leaf_count = 1;
            checkpoints[index].branch_leaf_examples.push_back(checkpoints[index].id);
            continue;
        }
        for (const auto child : children[index]) {
            checkpoints[index].branch_leaf_count += checkpoints[child].branch_leaf_count;
            const auto remaining = k_branch_leaf_example_limit - checkpoints[index].branch_leaf_examples.size();
            const auto copied = std::min(remaining, checkpoints[child].branch_leaf_examples.size());
            checkpoints[index].branch_leaf_examples.insert(checkpoints[index].branch_leaf_examples.end(),
                                                           checkpoints[child].branch_leaf_examples.begin(),
                                                           checkpoints[child].branch_leaf_examples.begin() + copied);
        }
    }

    std::vector<usize> roots;
    for (usize index = 0; index < checkpoints.size(); ++index) {
        if (!checkpoints[index].parent_checkpoint) roots.push_back(index);
    }
    std::vector<usize> pending;
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) pending.push_back(*root);
    std::vector<ConversationCheckpoint> ordered;
    ordered.reserve(checkpoints.size());
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        ordered.push_back(std::move(checkpoints[index]));
        for (auto child = children[index].rbegin(); child != children[index].rend(); ++child) pending.push_back(*child);
    }
    return ordered;
}

Result<Session> Session::fork_at(ConversationCheckpointId checkpoint) const {
    auto projected = conversation_checkpoints();
    if (!projected) return lighter::outcome_error(std::move(projected).error());
    if (std::ranges::none_of(*projected, [checkpoint](const ConversationCheckpoint &item) { return item.id == checkpoint; })) {
        return lighter::outcome_error(Error::protocol("requested conversation checkpoint is missing or unsafe"));
    }
    auto source_branch = branch_to(checkpoint.entry);
    if (!source_branch) return lighter::outcome_error(std::move(source_branch).error());

    Session fork;
    fork.metadata.workspace = metadata.workspace;
    fork.metadata.working_directory = metadata.working_directory;
    fork.metadata.preview = metadata.preview;
    fork.metadata.model_preference = metadata.model_preference;
    fork.metadata.forked_from = ForkOrigin{.session = id, .entry = checkpoint.entry};

    std::map<u64, TaskId> task_ids;
    std::map<u64, ProviderCallId> provider_call_ids;
    auto remap_task = [&task_ids](TaskId source) -> TaskId {
        if (const auto found = task_ids.find(source.value); found != task_ids.end()) return found->second;
        const TaskId mapped{.value = task_ids.size() + 1};
        task_ids.emplace(source.value, mapped);
        return mapped;
    };
    auto remap_call = [&provider_call_ids](ProviderCallId source) -> ProviderCallId {
        if (const auto found = provider_call_ids.find(source.value); found != provider_call_ids.end()) return found->second;
        const ProviderCallId mapped{.value = provider_call_ids.size() + 1};
        provider_call_ids.emplace(source.value, mapped);
        return mapped;
    };

    for (const auto *source : *source_branch) {
        auto payload = source->payload;
        std::visit(
            [&](auto &value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, TaskStarted> || std::same_as<T, TaskFinished>) {
                    value.id = remap_task(value.id);
                } else if constexpr (std::same_as<T, OutputItemCompleted> || std::same_as<T, ToolResults>) {
                    value.task_id = remap_task(value.task_id);
                    value.provider_call_id = remap_call(value.provider_call_id);
                } else if constexpr (std::same_as<T, ProviderCallCompleted> || std::same_as<T, ProviderCallAborted>) {
                    value.task_id = remap_task(value.task_id);
                    value.id = remap_call(value.id);
                }
            },
            payload);
        const EntryId id{.value = fork.next_entry_id++};
        fork.entries.push_back({
            .id = id,
            .parent_id = fork.active_leaf,
            .payload = std::move(payload),
            .created_at_ms = source->created_at_ms,
        });
        fork.active_leaf = id;
    }
    fork.next_task_id = task_ids.size() + 1;
    fork.next_provider_call_id = provider_call_ids.size() + 1;
    if (auto valid = fork.validate(); !valid) return lighter::outcome_error(std::move(valid).error());
    return fork;
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
