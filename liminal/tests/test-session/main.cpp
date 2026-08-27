#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <lighter/types.hpp>
#include <lighter/encoding/utf8.h>

#include <liminal/context/context.h>
#include <liminal/session/recovery.h>
#include <liminal/session/session.h>
#include <liminal/text.h>

namespace {

using namespace lighter::types;
using namespace liminal;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

session::EntryId append_message(session::Session &log, session::TaskId task_id, std::string text,
                                session::ProviderCallId call_id = {.value = 1},
                                provider::MessagePhase phase = provider::MessagePhase::UNSPECIFIED) {
    return log.append(session::OutputItemCompleted{
        .task_id = task_id,
        .provider_call_id = call_id,
        .item = provider::AssistantMessageItem{.id = {.value = "message"}, .parts = {{.text = std::move(text)}}, .phase = phase},
    });
}

session::ConversationCheckpointId checkpoint_id(session::EntryId entry) { return {entry}; }

void settle_round(session::Session &log, session::TaskId task_id, session::ProviderCallId call_id,
                  session::ProviderRoundReplay replay = session::ProviderRoundReplay::REPLAY) {
    log.append(session::ProviderRoundSettled{.task_id = task_id, .provider_call_id = call_id, .replay = replay});
}

void test_append_only_branching() {
    session::Session log;
    const auto first_task = log.start_task("first task");
    append_message(log, first_task, "first answer");
    const auto first_call = log.next_provider_call();
    log.append(session::ProviderCallCompleted{
        .task_id = first_task,
        .id = first_call,
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, first_task, first_call);
    const auto root = log.append(session::TaskFinished{.id = first_task});
    const auto original_task = log.start_task("original continuation");
    const auto original_leaf = log.append(session::TaskFinished{.id = original_task});

    auto selected = log.checkout(checkpoint_id(root));
    require(selected.has_value(), "failed to checkout a safe conversation checkpoint");
    const auto alternate_task = log.start_task("alternate task");
    const auto alternate_leaf = log.append(session::TaskFinished{.id = alternate_task});

    require(log.entries.size() == 9, "branching changed the session identity or deleted entries");
    require(log.find(original_leaf) != nullptr, "branching deleted the original leaf");
    require(log.entries[7].parent_id == root, "alternate branch has the wrong parent");

    auto branch = log.active_branch();
    require(branch.size() == 7 && branch.back()->id == alternate_leaf, "active branch did not follow parent links to the new leaf");
    auto manifest = context::ContextBuilder{}.build({}, log);
    require(manifest &&
                std::ranges::none_of(manifest->session_entries, [original_leaf](session::EntryId id) { return id == original_leaf; }) &&
                manifest->session_entries.back() == alternate_leaf,
            "provider context projection escaped the branch created after checkout");

    auto unsafe = log.checkout(checkpoint_id(session::EntryId{.value = root.value - 1}));
    require(!unsafe && unsafe.error().detail.contains("missing or unsafe"), "a mid-lifecycle entry was accepted as a checkpoint");
    auto invalid = log.checkout(checkpoint_id(session::EntryId{.value = 99}));
    require(!invalid && invalid.error().detail.contains("missing or unsafe"), "an unknown checkpoint was accepted");
}

void test_monotonic_timestamps_and_utf8_bounds() {
    session::Session log;
    const auto future = session::unix_milliseconds_now() + 60'000;
    log.metadata.created_at_ms = future;
    log.metadata.updated_at_ms = future;
    std::string prompt(239, 'a');
    prompt += "\xF0\x9F\x98\x80";
    const auto task = log.start_task(prompt);
    require(log.metadata.updated_at_ms == future && log.entries.back().created_at_ms == future,
            "wall-clock rollback moved a session mutation before its durable timestamp");
    require(log.metadata.preview.size() == 239 && lighter::encoding::utf8::is_valid(log.metadata.preview),
            "session preview split a UTF-8 code point at its byte bound");
    const auto checkpoint = log.append(session::TaskFinished{.id = task});
    require(log.checkout(checkpoint_id(checkpoint)).has_value() && log.metadata.updated_at_ms == future,
            "cursor mutation regressed the session timestamp");
    log.set_model_preference("provider", "model", std::nullopt);
    require(log.metadata.updated_at_ms == future && log.validate().has_value(),
            "model preference mutation produced an unloadable timestamp");
    log.set_title("Named session");
    require(log.metadata.title == "Named session" && log.metadata.updated_at_ms == future, "rename advanced conversation recency");
    const auto older_task = log.start_task("older admission", future - 1);
    require(log.metadata.updated_at_ms == future, "an older task admission regressed conversation recency");
    log.append(session::TaskFinished{.id = older_task});
    log.start_task("newer admission", future + 1);
    require(log.metadata.updated_at_ms == future + 1 && log.validate().has_value(),
            "a newer task admission did not advance conversation recency");

    std::string diagnostic(4095, 'x');
    diagnostic += "\xF0\x9F\x98\x80";
    const auto bounded = bounded_utf8(diagnostic, 4096);
    require(bounded.size() == 4095 && lighter::encoding::utf8::is_valid(bounded), "provider diagnostic bound emitted malformed UTF-8");

    const std::string invalid(240, static_cast<char>(0x80));
    const auto sanitized = bounded_utf8(invalid, 240);
    require(sanitized.size() <= 240 && lighter::encoding::utf8::is_valid(sanitized),
            "UTF-8 sanitization expanded text beyond its advertised byte bound");
}

void test_checkpoint_projection() {
    session::Session log;
    const auto old_task = log.start_task("old task");
    append_message(log, old_task, "old answer");

    session::ContextCheckpoint checkpoint;
    checkpoint.items.push_back(session::ContextInput{{provider::TextPart{.text = "summary"}}});
    checkpoint.items.push_back(session::CheckpointOutput{
        .item = provider::AssistantMessageItem{.id = {.value = "checkpoint"}, .parts = {{.text = "preserved answer"}}},
    });
    const auto checkpoint_id = log.append(std::move(checkpoint));
    log.start_task("continue");
    const auto recent_id = *log.active_leaf;

    auto manifest = context::ContextBuilder{}.build({}, log);
    require(manifest.has_value(), "failed to project a checkpointed session");
    require(log.entries.size() == 4, "checkpoint projection mutated the session log");
    require(manifest->omitted_session_entries == 2 && manifest->session_entries.size() == 2 &&
                manifest->session_entries[0] == checkpoint_id && manifest->session_entries[1] == recent_id,
            "manifest did not expose the checkpoint context boundary");
    require(manifest->provider_history.size() == 3 && manifest->provider_history[0].role == provider::Role::USER &&
                manifest->provider_history[1].role == provider::Role::ASSISTANT &&
                manifest->provider_history[2].role == provider::Role::USER,
            "checkpoint items were lowered with incorrect provider roles");
}

void test_cumulative_token_usage() {
    session::Session log;
    const auto first_task = log.start_task("first");
    const auto first_call = log.next_provider_call();
    log.append(session::ProviderCallCompleted{
        .task_id = first_task,
        .id = first_call,
        .completion = {.usage = {.input_tokens = 30, .output_tokens = 5, .context_tokens = 40}},
    });
    settle_round(log, first_task, first_call);
    const auto first = log.append(session::TaskFinished{.id = first_task});
    const auto second_task = log.start_task("continue");
    const auto second_call = log.next_provider_call();
    log.append(session::ProviderCallCompleted{
        .task_id = second_task,
        .id = second_call,
        .completion = {.usage = {.input_tokens = 50, .output_tokens = 7}},
    });
    settle_round(log, second_task, second_call);
    log.append(session::TaskFinished{.id = second_task});

    require(log.tokens_used() == 97, "session token usage did not prefer normalized response totals");

    auto selected = log.checkout(checkpoint_id(first));
    require(selected.has_value() && log.tokens_used() == 97, "branch selection changed tokens already consumed by the session");
}

void test_provider_round_coherence_validation() {
    session::Session incomplete;
    const auto task = incomplete.start_task("inspect");
    const auto call = incomplete.next_provider_call();
    incomplete.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "tool"}, .call = {.id = "call", .name = "read_file"}},
    });
    incomplete.append(session::ProviderCallCompleted{
        .task_id = task,
        .id = call,
        .loop_outcome = session::ProviderCallLoopOutcome::FOLLOW_UP,
    });
    incomplete.append(session::ProviderRoundSettled{
        .task_id = task,
        .provider_call_id = call,
        .replay = session::ProviderRoundReplay::REPLAY,
    });
    require(!incomplete.validate(), "session validation accepted a replayed tool call without an outcome");

    session::Session coherent;
    const auto coherent_task = coherent.start_task("inspect");
    const auto coherent_call = coherent.next_provider_call();
    coherent.append(session::OutputItemCompleted{
        .task_id = coherent_task,
        .provider_call_id = coherent_call,
        .item = provider::ToolCallItem{.id = {.value = "tool"}, .call = {.id = "call", .name = "read_file"}},
    });
    coherent.append(session::ProviderCallCompleted{
        .task_id = coherent_task,
        .id = coherent_call,
        .loop_outcome = session::ProviderCallLoopOutcome::FOLLOW_UP,
    });
    coherent.append(session::ToolOutcomes{
        .task_id = coherent_task,
        .provider_call_id = coherent_call,
        .outcomes = {{.call_id = "call", .kind = ToolOutcomeKind::NOT_STARTED, .receipt = "reason: fixture"}},
    });
    settle_round(coherent, coherent_task, coherent_call);
    coherent.append(session::TaskFinished{.id = coherent_task, .outcome = session::TaskOutcome::INTERRUPTED});
    require(coherent.validate().has_value(), "session validation rejected an exactly settled provider round");

    session::Session failed_replay;
    const auto failed_task = failed_replay.start_task("reject failed replay");
    const auto failed_call = failed_replay.next_provider_call();
    append_message(failed_replay, failed_task, "partial output", failed_call);
    failed_replay.append(session::ProviderCallCompleted{
        .task_id = failed_task,
        .id = failed_call,
        .completion = {.stop = provider::StopKind::TRUNCATED},
        .loop_outcome = session::ProviderCallLoopOutcome::FAILED,
    });
    settle_round(failed_replay, failed_task, failed_call, session::ProviderRoundReplay::REPLAY);
    failed_replay.append(session::TaskFinished{.id = failed_task, .outcome = session::TaskOutcome::FAILED});
    require(!failed_replay.validate(), "session validation accepted replay of a failed provider round");
}

void test_recovery_omits_failed_completed_provider_round() {
    session::Session log;
    const auto task = log.start_task("recover failed round");
    const auto call = log.next_provider_call();
    log.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "tool"}, .call = {.id = "failed-call", .name = "read_file"}},
    });
    log.append(session::ProviderCallCompleted{
        .task_id = task,
        .id = call,
        .completion = {.stop = provider::StopKind::TRUNCATED},
        .loop_outcome = session::ProviderCallLoopOutcome::FAILED,
    });

    const auto recovered = session::recover_interrupted(log);
    require(recovered.recovered_tasks == 1 && recovered.unknown_tool_outcomes == 1,
            "recovery did not close the interrupted failed provider round");
    const auto settled = std::ranges::find_if(log.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ProviderRoundSettled>(entry.payload);
    });
    require(settled != log.entries.end() &&
                std::get<session::ProviderRoundSettled>(settled->payload).replay == session::ProviderRoundReplay::OMIT,
            "recovery replayed a provider round already classified as failed");
    require(log.validate().has_value(), "recovered failed provider round is not durable-session coherent");
}

void test_reply_selection() {
    session::Session log;
    const auto first_task = log.start_task("first");
    append_message(log, first_task, "first answer");
    log.append(session::ProviderCallCompleted{
        .task_id = first_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, first_task, {.value = 1});
    log.append(session::TaskFinished{.id = first_task});
    const auto first_leaf = *log.active_leaf;
    const auto second_task = log.start_task("second");
    append_message(log, second_task, "checking", {.value = 2}, provider::MessagePhase::COMMENTARY);
    append_message(log, second_task, "premature answer", {.value = 2}, provider::MessagePhase::FINAL);
    log.append(session::OutputItemCompleted{
        .task_id = second_task,
        .provider_call_id = {.value = 2},
        .item =
            provider::ToolCallItem{
                .id = {.value = "tool"},
                .call = {.id = "call", .name = "read_file"},
            },
    });
    log.append(session::ProviderCallCompleted{
        .task_id = second_task,
        .id = {.value = 2},
        .loop_outcome = session::ProviderCallLoopOutcome::FOLLOW_UP,
    });
    log.append(session::ToolOutcomes{
        .task_id = second_task,
        .provider_call_id = {.value = 2},
        .outcomes = {{.call_id = "call", .kind = ToolOutcomeKind::FAILED, .receipt = "reason: fixture"}},
    });
    settle_round(log, second_task, {.value = 2});
    log.append(session::OutputItemCompleted{
        .task_id = second_task,
        .provider_call_id = {.value = 3},
        .item =
            provider::ProviderOpaqueItem{
                .id = {.value = "opaque"},
                .part = {.provider_tag = "test", .payload = "private"},
            },
    });
    append_message(log, second_task, "terminal commentary", {.value = 3}, provider::MessagePhase::COMMENTARY);
    append_message(log, second_task, "unphased fallback", {.value = 3});
    append_message(log, second_task, "second answer", {.value = 3}, provider::MessagePhase::FINAL);
    log.append(session::ProviderCallCompleted{
        .task_id = second_task,
        .id = {.value = 3},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, second_task, {.value = 3});
    log.append(session::TaskFinished{.id = second_task});

    require(log.reply_from_latest() == "second answer", "latest reply did not select only the explicit final answer");
    require(log.reply_from_latest(2) == "first answer", "older reply selection used the wrong newest-first ordinal");
    require(!log.reply_from_latest(0) && !log.reply_from_latest(3), "reply selection accepted an invalid ordinal");

    auto selected = log.checkout(checkpoint_id(first_leaf));
    require(selected.has_value() && log.reply_from_latest() == "first answer", "reply selection escaped the active session branch");

    const auto failed_task = log.start_task("failed");
    append_message(log, failed_task, "unfinished output");
    log.append(session::ProviderCallCompleted{
        .task_id = failed_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, failed_task, {.value = 1}, session::ProviderRoundReplay::OMIT);
    log.append(session::TaskFinished{.id = failed_task, .outcome = session::TaskOutcome::FAILED});
    require(log.reply_from_latest() == "first answer", "failed task displaced the newest completed reply");

    const auto cancelled_task = log.start_task("cancelled");
    append_message(log, cancelled_task, "cancelled output");
    log.append(session::ProviderCallCompleted{
        .task_id = cancelled_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, cancelled_task, {.value = 1}, session::ProviderRoundReplay::OMIT);
    log.append(session::TaskFinished{.id = cancelled_task, .outcome = session::TaskOutcome::CANCELLED});
    require(log.reply_from_latest() == "first answer", "cancelled task displaced the newest completed reply");
}

void test_checkpoint_tree_projection() {
    session::Session log;
    const auto first_task = log.start_task("shared root");
    const auto unsafe_output = append_message(log, first_task, "root answer");
    const auto first_call = log.next_provider_call();
    log.append(session::ProviderCallCompleted{
        .task_id = first_task,
        .id = first_call,
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(log, first_task, first_call);
    const auto first = log.append(session::TaskFinished{.id = first_task});

    const auto old_task = log.start_task("old branch");
    const auto old_leaf = log.append(session::TaskFinished{.id = old_task});
    require(log.checkout(checkpoint_id(first)).has_value(), "failed to rewind before projecting a branch tree");
    const auto active_task = log.start_task("active branch");
    session::ContextCheckpoint within_task_compaction;
    within_task_compaction.items.push_back(session::ContextInput{{provider::TextPart{.text = "automatic summary"}}});
    const auto unsafe_compaction = log.append(std::move(within_task_compaction));
    const auto active_task_leaf = log.append(session::TaskFinished{.id = active_task});
    session::ContextCheckpoint compacted;
    compacted.items.push_back(session::ContextInput{{provider::TextPart{.text = "summary"}}});
    const auto active_leaf = log.append(std::move(compacted));

    auto checkpoints = log.conversation_checkpoints();
    require(checkpoints.has_value() && checkpoints->size() == 4, "safe checkpoint projection omitted a completed boundary");
    require((*checkpoints)[0].id == checkpoint_id(first) && (*checkpoints)[0].direct_descendants == 2 &&
                (*checkpoints)[0].branch_leaf_count == 2 &&
                (*checkpoints)[0].branch_leaf_examples ==
                    std::vector<session::ConversationCheckpointId>{checkpoint_id(old_leaf), checkpoint_id(active_leaf)},
            "checkpoint projection did not identify both descendant branch leaves");
    require(!(*checkpoints)[1].on_active_branch && (*checkpoints)[1].id == checkpoint_id(old_leaf),
            "preserved branch was incorrectly marked active");
    require((*checkpoints)[2].id == checkpoint_id(active_task_leaf) && (*checkpoints)[2].on_active_branch &&
                (*checkpoints)[3].id == checkpoint_id(active_leaf) && (*checkpoints)[3].active,
            "active ancestry or append point was projected incorrectly");
    require(!log.checkout(checkpoint_id(unsafe_output)).has_value(), "provider output was exposed as a safe checkpoint");
    require(!log.checkout(checkpoint_id(unsafe_compaction)).has_value(), "within-task compaction was exposed as a safe idle checkpoint");
}

void test_checkpoint_branch_summaries_are_bounded() {
    constexpr usize k_branch_levels = 2'000;
    session::Session log;
    const auto root_task = log.start_task("comb root");
    const auto root = log.append(session::TaskFinished{.id = root_task});
    auto spine = root;
    for (usize level = 0; level < k_branch_levels; ++level) {
        log.active_leaf = spine;
        const auto side_task = log.start_task("side branch");
        log.append(session::TaskFinished{.id = side_task});

        log.active_leaf = spine;
        const auto spine_task = log.start_task("spine branch");
        spine = log.append(session::TaskFinished{.id = spine_task});
    }

    auto checkpoints = log.conversation_checkpoints();
    require(checkpoints && checkpoints->size() == 1 + 2 * k_branch_levels, "large comb tree did not project every safe checkpoint");
    require(checkpoints->front().id == checkpoint_id(root) && checkpoints->front().branch_leaf_count == k_branch_levels + 1 &&
                checkpoints->front().branch_leaf_examples.size() == session::k_branch_leaf_example_limit,
            "large comb root did not retain an exact count and bounded examples");
    usize retained_examples = 0;
    for (const auto &checkpoint : *checkpoints) {
        require(checkpoint.branch_leaf_examples.size() <= session::k_branch_leaf_example_limit &&
                    checkpoint.branch_leaf_examples.size() <= checkpoint.branch_leaf_count,
                "checkpoint retained an unbounded or incoherent branch summary");
        retained_examples += checkpoint.branch_leaf_examples.size();
    }
    require(retained_examples <= checkpoints->size() * session::k_branch_leaf_example_limit,
            "branch projection storage did not remain linear in checkpoint count");
}

void test_fork_remaps_lifecycle_and_preserves_private_items() {
    session::Session source;
    source.metadata.title = "Source name";
    source.metadata.workspace = session::SessionWorkspace{.root = "D:/workspace", .key = "workspace-key"};
    source.metadata.working_directory = "D:/workspace/subdir";
    source.set_model_preference("provider", "model", "high");

    const auto first_task = source.start_task("shared root");
    const auto first_call = source.next_provider_call();
    append_message(source, first_task, "root answer", first_call);
    source.append(session::ProviderCallCompleted{
        .task_id = first_task,
        .id = first_call,
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(source, first_task, first_call);
    const auto first = source.append(session::TaskFinished{.id = first_task});

    const auto discarded_task = source.start_task("discarded branch");
    const auto discarded_call = source.next_provider_call();
    source.append(session::ProviderCallAborted{.task_id = discarded_task, .id = discarded_call});
    settle_round(source, discarded_task, discarded_call, session::ProviderRoundReplay::OMIT);
    source.append(session::TaskFinished{.id = discarded_task, .outcome = session::TaskOutcome::FAILED});
    require(source.checkout(checkpoint_id(first)).has_value(), "failed to select fork source branch");

    const auto selected_task = source.start_task("selected branch");
    const auto selected_call = source.next_provider_call();
    source.append(session::OutputItemCompleted{
        .task_id = selected_task,
        .provider_call_id = selected_call,
        .item =
            provider::ProviderOpaqueItem{
                .id = {.value = "opaque-id"},
                .part = {.provider_tag = "test-provider", .payload = R"({"private":true})"},
            },
    });
    source.append(session::ProviderCallCompleted{
        .task_id = selected_task,
        .id = selected_call,
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    settle_round(source, selected_task, selected_call);
    const auto selected = source.append(session::TaskFinished{.id = selected_task});

    auto fork = source.fork_at(checkpoint_id(selected));
    require(fork.has_value(), "failed to fork a safe checkpoint");
    require(fork->id != source.id && fork->metadata.forked_from == session::ForkOrigin{source.id, selected},
            "fork identity or exact provenance is incorrect");
    require(!fork->metadata.title && fork->metadata.workspace == source.metadata.workspace &&
                fork->metadata.working_directory == source.metadata.working_directory,
            "fork catalog metadata did not follow copy policy");
    require(fork->entries.size() == 10 && fork->next_entry_id == 11 && fork->next_task_id == 3 && fork->next_provider_call_id == 3,
            "fork did not produce dense local lifecycle identifiers");
    for (usize index = 0; index < fork->entries.size(); ++index) {
        require(fork->entries[index].id.value == index + 1, "fork entry identifiers were not remapped into a dense local sequence");
    }
    const auto *opaque = std::get_if<session::OutputItemCompleted>(&fork->entries[6].payload);
    require(opaque && opaque->task_id.value == 2 && opaque->provider_call_id.value == 2,
            "fork payload lifecycle references were not remapped");
    const auto *private_item = opaque ? std::get_if<provider::ProviderOpaqueItem>(&opaque->item) : nullptr;
    require(private_item && private_item->id.value == "opaque-id" && private_item->part.provider_tag == "test-provider" &&
                private_item->part.payload == R"({"private":true})",
            "fork did not preserve the provider-private item");
    require(fork->validate().has_value(), "fork lifecycle counters are incoherent");
}

i32 run_all() {
    test_append_only_branching();
    test_monotonic_timestamps_and_utf8_bounds();
    test_checkpoint_projection();
    test_cumulative_token_usage();
    test_provider_round_coherence_validation();
    test_recovery_omits_failed_completed_provider_round();
    test_reply_selection();
    test_checkpoint_tree_projection();
    test_checkpoint_branch_summaries_are_bounded();
    test_fork_remaps_lifecycle_and_preserves_private_items();
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
