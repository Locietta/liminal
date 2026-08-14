#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <lighter/types.hpp>
#include <lighter/encoding/utf8.h>

#include <liminal/context/context.h>
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

void test_append_only_branching() {
    session::Session log;
    const auto first_task = log.start_task("first task");
    const auto root = session::EntryId{.value = 1};
    const auto original_leaf = append_message(log, first_task, "first answer");

    auto selected = log.select_leaf(root);
    require(selected.has_value(), "failed to select a known session entry");
    log.start_task("alternate task");
    const auto alternate_leaf = *log.active_leaf;

    require(log.entries.size() == 3, "branching changed the session identity or deleted entries");
    require(log.find(original_leaf) != nullptr, "branching deleted the original leaf");
    require(log.entries.back().parent_id == root, "alternate branch has the wrong parent");

    auto branch = log.active_branch();
    require(branch.size() == 2 && branch[0]->id == root && branch[1]->id == alternate_leaf,
            "active branch did not follow parent links from root to leaf");

    auto invalid = log.select_leaf(session::EntryId{.value = 99});
    require(!invalid && invalid.error().detail.contains("unknown session entry"), "an unknown branch leaf was accepted");
}

void test_monotonic_timestamps_and_utf8_bounds() {
    session::Session log;
    const auto future = session::unix_milliseconds_now() + 60'000;
    log.metadata.created_at_ms = future;
    log.metadata.updated_at_ms = future;
    std::string prompt(239, 'a');
    prompt += "\xF0\x9F\x98\x80";
    log.start_task(prompt);
    require(log.metadata.updated_at_ms == future && log.entries.back().created_at_ms == future,
            "wall-clock rollback moved a session mutation before its durable timestamp");
    require(log.metadata.preview.size() == 239 && lighter::encoding::utf8::is_valid(log.metadata.preview),
            "session preview split a UTF-8 code point at its byte bound");
    require(log.select_leaf(std::nullopt).has_value() && log.metadata.updated_at_ms == future,
            "cursor mutation regressed the session timestamp");
    log.set_model_preference("provider", "model", std::nullopt);
    require(log.metadata.updated_at_ms == future && log.validate().has_value(),
            "model preference mutation produced an unloadable timestamp");
    log.set_title("Named session");
    log.archive();
    require(log.metadata.title == "Named session" && log.metadata.archived_at_ms == future && log.metadata.updated_at_ms == future,
            "title or archive mutation bypassed the session timestamp boundary");
    log.unarchive();
    require(!log.metadata.archived_at_ms && log.metadata.updated_at_ms == future && log.validate().has_value(),
            "unarchive mutation produced invalid session metadata");

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
    const auto first = log.append(session::ProviderCallCompleted{
        .completion = {.usage = {.input_tokens = 30, .output_tokens = 5, .context_tokens = 40}},
    });
    log.start_task("continue");
    log.append(session::ProviderCallCompleted{.completion = {.usage = {.input_tokens = 50, .output_tokens = 7}}});

    require(log.tokens_used() == 97, "session token usage did not prefer normalized response totals");

    auto selected = log.select_leaf(first);
    require(selected.has_value() && log.tokens_used() == 97, "branch selection changed tokens already consumed by the session");
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
    log.append(session::ToolResults{.task_id = second_task, .provider_call_id = {.value = 2}});
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
    log.append(session::TaskFinished{.id = second_task});

    require(log.reply_from_latest() == "second answer", "latest reply did not select only the explicit final answer");
    require(log.reply_from_latest(2) == "first answer", "older reply selection used the wrong newest-first ordinal");
    require(!log.reply_from_latest(0) && !log.reply_from_latest(3), "reply selection accepted an invalid ordinal");

    auto selected = log.select_leaf(first_leaf);
    require(selected.has_value() && log.reply_from_latest() == "first answer", "reply selection escaped the active session branch");

    const auto failed_task = log.start_task("failed");
    append_message(log, failed_task, "unfinished output");
    log.append(session::ProviderCallCompleted{
        .task_id = failed_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    log.append(session::TaskFinished{.id = failed_task, .outcome = session::TaskOutcome::FAILED});
    require(log.reply_from_latest() == "first answer", "failed task displaced the newest completed reply");

    const auto cancelled_task = log.start_task("cancelled");
    append_message(log, cancelled_task, "cancelled output");
    log.append(session::ProviderCallCompleted{
        .task_id = cancelled_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    log.append(session::TaskFinished{.id = cancelled_task, .outcome = session::TaskOutcome::CANCELLED});
    require(log.reply_from_latest() == "first answer", "cancelled task displaced the newest completed reply");
}

i32 run_all() {
    test_append_only_branching();
    test_monotonic_timestamps_and_utf8_bounds();
    test_checkpoint_projection();
    test_cumulative_token_usage();
    test_reply_selection();
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
