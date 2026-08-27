#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/async/vocab/outcome.h>
#include <lighter/mock/mock.h>
#include <lighter/types.hpp>

#include <liminal/context/context.h>
#include <liminal/context/project_instructions.h>
#include <liminal/session/session.h>

namespace {

using namespace lighter::types;
using namespace liminal;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void append_message(session::Session &log, session::TaskId task_id, std::string text, session::ProviderCallId call_id = {.value = 1}) {
    log.append(session::OutputItemCompleted{
        .task_id = task_id,
        .provider_call_id = call_id,
        .item = provider::AssistantMessageItem{.id = {.value = "message"}, .parts = {{.text = std::move(text)}}},
    });
}

void append_tool_call(session::Session &log, session::TaskId task_id, std::string id, std::string name,
                      session::ProviderCallId call_id = {.value = 1}) {
    log.append(session::OutputItemCompleted{
        .task_id = task_id,
        .provider_call_id = call_id,
        .item = provider::ToolCallItem{.id = {.value = "item-" + id}, .call = {.id = std::move(id), .name = std::move(name)}},
    });
}

void complete_round(session::Session &log, session::TaskId task_id, session::ProviderCallId call_id,
                    session::ProviderCallLoopOutcome outcome = session::ProviderCallLoopOutcome::TERMINAL, provider::Usage usage = {}) {
    log.append(session::ProviderCallCompleted{.task_id = task_id, .id = call_id, .completion = {.usage = usage}, .loop_outcome = outcome});
    log.append(session::ProviderRoundSettled{
        .task_id = task_id,
        .provider_call_id = call_id,
        .replay = session::ProviderRoundReplay::REPLAY,
    });
}

std::vector<context::InstructionSource> resolve_fixture() {
    const std::filesystem::path root = "workspace";
    const auto active = root / "src" / "module";

    lighter::mock::Mock<context::InstructionFilesFacade> files;
    files.expect<context::CanonicalizeInstructionPathDispatch>()
        .then_calls([&](const std::filesystem::path &path) -> Result<std::filesystem::path> {
            require(path == root, "resolver canonicalized the wrong workspace root");
            return root;
        })
        .then_calls([&](const std::filesystem::path &path) -> Result<std::filesystem::path> {
            require(path == active, "resolver canonicalized the wrong active directory");
            return active;
        });
    files.expect<context::ReadInstructionFileDispatch>()
        .then_calls([&](const std::filesystem::path &path) -> Result<std::string> {
            require(path == root / "AGENTS.md", "resolver did not read the root instruction first");
            return std::string("root policy");
        })
        .then_calls([&](const std::filesystem::path &path) -> Result<std::string> {
            require(path == root / "src" / "AGENTS.md", "resolver did not inspect the intermediate scope");
            return lighter::outcome_error(Error::config("not found", ErrorCode::NOT_FOUND));
        })
        .then_calls([&](const std::filesystem::path &path) -> Result<std::string> {
            require(path == active / "AGENTS.md", "resolver did not read the narrow instruction last");
            return std::string("module policy");
        });

    auto handle = files.handle();
    auto resolved = context::ProjectInstructionResolver{}.resolve(root, active, handle);
    require(resolved.has_value(), "resolver rejected a valid instruction hierarchy");
    files.verify();
    return *std::move(resolved);
}

void test_resolution_order_scope_and_determinism() {
    const auto first = resolve_fixture();
    const auto second = resolve_fixture();

    require(first.size() == 2 && second.size() == first.size(), "resolver did not omit a missing instruction file");
    require(first[0].content == "root policy" && first[0].scope == std::filesystem::path("workspace"),
            "root instruction has the wrong content or scope");
    require(first[1].content == "module policy" && first[1].scope == std::filesystem::path("workspace/src/module"),
            "nested instruction has the wrong content or scope");
    require(first[0].authority == context::InstructionAuthority::PROJECT && first[0].trust == context::InstructionTrust::WORKSPACE,
            "project instructions have the wrong authority or trust");
    require(first[0].origin == second[0].origin && first[0].scope == second[0].scope && first[0].content == second[0].content &&
                first[1].origin == second[1].origin && first[1].scope == second[1].scope && first[1].content == second[1].content,
            "identical filesystem views must produce deterministic instructions");
}

void test_workspace_boundary() {
    lighter::mock::Mock<context::InstructionFilesFacade> files;
    files.expect<context::CanonicalizeInstructionPathDispatch>()
        .then_calls([](const std::filesystem::path &) -> Result<std::filesystem::path> { return std::filesystem::path("workspace"); })
        .then_calls([](const std::filesystem::path &) -> Result<std::filesystem::path> { return std::filesystem::path("elsewhere/task"); });
    files.expect<context::ReadInstructionFileDispatch>().never();

    auto handle = files.handle();
    auto resolved = context::ProjectInstructionResolver{}.resolve("workspace", "elsewhere/task", handle);
    require(!resolved && resolved.error().kind == ErrorKind::CONFIG && resolved.error().detail.contains("outside project"),
            "resolver accepted an active directory outside its project");
    files.verify();
}

void test_project_root_discovery() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() / ("liminal-context-" + std::to_string(nonce));
    const auto repository = base / "repository";
    const auto nested = repository / "src" / "module";
    const auto standalone = base / "standalone";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directories(standalone);
    {
        std::ofstream marker(repository / ".git", std::ios::binary);
        marker << "gitdir: elsewhere";
        require(static_cast<bool>(marker), "failed to create Git marker");
    }

    auto discovered = context::discover_project_root(nested);
    require(discovered && std::filesystem::equivalent(*discovered, repository),
            "project discovery did not find the nearest enclosing Git root: " +
                (discovered ? discovered->string() : discovered.error().message()));
    auto fallback = context::discover_project_root(standalone);
    require(fallback && *fallback == std::filesystem::weakly_canonical(standalone),
            "project discovery did not fall back to the invocation directory");

    std::error_code remove_error;
    std::filesystem::remove_all(base, remove_error);
}

void test_instruction_read_failure() {
    lighter::mock::Mock<context::InstructionFilesFacade> files;
    files.expect<context::CanonicalizeInstructionPathDispatch>()
        .then_calls([](const std::filesystem::path &) -> Result<std::filesystem::path> { return std::filesystem::path("workspace"); })
        .then_calls([](const std::filesystem::path &) -> Result<std::filesystem::path> { return std::filesystem::path("workspace"); });
    files.expect<context::ReadInstructionFileDispatch>().calls([](const std::filesystem::path &) -> Result<std::string> {
        return lighter::outcome_error(Error::config("permission denied", ErrorCode::IO));
    });

    auto handle = files.handle();
    auto resolved = context::ProjectInstructionResolver{}.resolve("workspace", "workspace", handle);
    require(!resolved && resolved.error().detail == "permission denied", "resolver hid or replaced an instruction read failure");
    files.verify();
}

void test_instruction_read_is_strict() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("liminal-missing-instructions-" + std::to_string(nonce)) / "AGENTS.md";
    auto content = context::LocalInstructionFiles{}.read_instruction_file(path);
    require(!content && content.error().code == ErrorCode::NOT_FOUND, "a missing instruction file must be a structured failed read");
}

void test_manifest_deduplication_and_redacted_description() {
    std::vector<context::InstructionSource> instructions{
        {
            .authority = context::InstructionAuthority::APPLICATION,
            .trust = context::InstructionTrust::PLATFORM,
            .origin = "builtin:test",
            .content = "SECRET POLICY",
        },
        {
            .authority = context::InstructionAuthority::PROJECT,
            .trust = context::InstructionTrust::WORKSPACE,
            .origin = "project:workspace/AGENTS.md",
            .scope = "workspace",
            .content = "SECRET POLICY",
        },
    };
    session::Session session_log;
    session_log.start_task("OLD SECRET");
    session::ContextCheckpoint checkpoint;
    checkpoint.items.push_back(session::ContextInput{.parts = {provider::TextPart{.text = "SUMMARY SECRET"}}});
    const auto checkpoint_id = session_log.append(std::move(checkpoint));
    session_log.start_task("CURRENT SECRET");

    auto manifest = context::ContextBuilder{}.build(
        instructions, session_log, {.context_window_tokens = 100, .reserved_output_tokens = 10, .safety_margin_tokens = 5});
    require(manifest.has_value(), "context builder rejected a valid manifest");
    require(manifest->instructions.size() == 1 && manifest->omitted_duplicates.size() == 1,
            "context builder did not deduplicate identical instruction content");
    require(manifest->omitted_session_entries == 1 && manifest->session_entries.size() == 2 && manifest->active_checkpoint == checkpoint_id,
            "context manifest did not expose checkpoint selection");
    require(manifest->usage.instruction_bytes == std::string_view("SECRET POLICY").size() &&
                manifest->usage.conversation_bytes == std::string_view("SUMMARY SECRETCURRENT SECRET").size() &&
                manifest->usage.input_budget_tokens == 85 && manifest->usage.remaining_input_tokens.has_value(),
            "context manifest reported the wrong budget breakdown");

    const auto description = context::describe(*manifest);
    require(description.contains("builtin:test") && description.contains("project:workspace/AGENTS.md") &&
                description.contains("active checkpoint: 2") && description.contains("session entries selected: 2"),
            "context description omitted provenance or selection metadata");
    require(description.contains("input budget: 85 tokens") && description.contains("payload bytes: instructions 13"),
            "context description omitted model budget metadata");
    require(!description.contains("SECRET POLICY") && !description.contains("SUMMARY SECRET") && !description.contains("CURRENT SECRET") &&
                !description.contains("OLD SECRET"),
            "context description exposed instruction or session contents");
}

void test_bounded_selection_keeps_complete_recent_tasks() {
    session::Session session_log;
    const auto old_task = session_log.start_task(std::string(80, 'a'));
    append_message(session_log, old_task, std::string(80, 'b'));
    complete_round(session_log, old_task, {.value = 1});
    session_log.append(session::TaskFinished{.id = old_task});
    const auto tool_task = session_log.start_task("use a tool");
    append_tool_call(session_log, tool_task, "call", "read");
    session_log.append(session::ProviderCallCompleted{
        .task_id = tool_task,
        .id = {.value = 1},
        .loop_outcome = session::ProviderCallLoopOutcome::FOLLOW_UP,
    });
    session_log.append(session::ToolOutcomes{
        .task_id = tool_task,
        .provider_call_id = {.value = 1},
        .outcomes = {{.call_id = "call", .payload = std::string(80, 'r')}},
    });
    session_log.append(session::ProviderRoundSettled{
        .task_id = tool_task,
        .provider_call_id = {.value = 1},
        .replay = session::ProviderRoundReplay::REPLAY,
    });
    append_message(session_log, tool_task, "tool conclusion", {.value = 2});
    complete_round(session_log, tool_task, {.value = 2});
    session_log.append(session::TaskFinished{.id = tool_task});
    const auto latest_task = session_log.start_task("latest");
    append_message(session_log, latest_task, "answer", {.value = 3});
    complete_round(session_log, latest_task, {.value = 3});
    session_log.append(session::TaskFinished{.id = latest_task});

    auto manifest = context::ContextBuilder{}.build({}, session_log,
                                                    {.context_window_tokens = 60, .reserved_output_tokens = 10, .safety_margin_tokens = 0});
    require(manifest.has_value(), "bounded context rejected a valid recent task");
    require(manifest->session_entries.size() == 5 && manifest->session_entries[0].value == 15 &&
                manifest->session_entries.back().value == 19,
            "bounded context did not retain the newest complete task");
    require(manifest->omitted_budget_entries == 14 && manifest->omitted_checkpoint_entries == 0,
            "bounded context did not report budget omissions separately");
    require(manifest->provider_history.size() == 2 && manifest->usage.remaining_input_tokens >= 0,
            "bounded context emitted an oversized or partial provider history");
}

void test_reported_usage_accounts_for_trailing_context() {
    session::Session session_log;
    const auto task_id = session_log.start_task(std::string(200, 'u'));
    append_message(session_log, task_id, std::string(200, 'a'));
    append_tool_call(session_log, task_id, "c", "read");
    session_log.append(session::ProviderCallCompleted{
        .task_id = task_id,
        .id = {.value = 1},
        .completion = {.usage = {.input_tokens = 50, .output_tokens = 20, .context_tokens = 70}},
    });
    session_log.append(session::ToolOutcomes{
        .task_id = task_id,
        .provider_call_id = {.value = 1},
        .outcomes = {{.call_id = "c", .payload = std::string(15, 'r')}},
    });
    session_log.append(session::ProviderRoundSettled{
        .task_id = task_id,
        .provider_call_id = {.value = 1},
        .replay = session::ProviderRoundReplay::REPLAY,
    });
    session_log.append(session::TaskFinished{.id = task_id});
    session_log.start_task("tail");

    auto manifest = context::ContextBuilder{}.build({}, session_log, {.context_window_tokens = 100});
    require(manifest.has_value(), "reported usage rejected a valid context");
    require(manifest->usage.reported_context_tokens == 70 && manifest->usage.estimated_trailing_tokens == 30 &&
                manifest->usage.estimated_input_tokens == 100 && manifest->usage.remaining_input_tokens == 0,
            "context accounting did not combine reported usage with its estimated tail: reported " +
                std::to_string(manifest->usage.reported_context_tokens.value_or(0)) + ", trailing " +
                std::to_string(manifest->usage.estimated_trailing_tokens) + ", input " +
                std::to_string(manifest->usage.estimated_input_tokens) + ", remaining " +
                std::to_string(manifest->usage.remaining_input_tokens.value_or(0)));
    require(context::describe(*manifest).contains("reported context baseline: 70 tokens"),
            "context inspection omitted its reported usage baseline");
}

void test_projected_tool_outcomes_charge_exact_envelope_overhead() {
    session::Session session_log;
    const auto task_id = session_log.start_task("use tools");
    append_tool_call(session_log, task_id, "call0001", "first");
    append_tool_call(session_log, task_id, "call0002", "second");
    session_log.append(session::ProviderCallCompleted{
        .task_id = task_id,
        .id = {.value = 1},
        .completion = {.usage = {.context_tokens = 100}},
    });
    const std::vector<ToolOutcome> empty_results{{.call_id = "call0001"}, {.call_id = "call0002"}};
    const std::vector<ToolOutcome> payload_results{{.call_id = "call0001", .payload = std::string(40, 'x')},
                                                   {.call_id = "call0002", .payload = std::string(40, 'x')}};

    auto base = context::ContextBuilder{}.build({}, session_log, {.context_window_tokens = 1'000}, empty_results);
    auto projected = context::ContextBuilder{}.build({}, session_log, {.context_window_tokens = 1'000}, payload_results);

    require(base && projected && projected->usage.estimated_input_tokens > base->usage.estimated_input_tokens &&
                projected->usage.tool_bytes == base->usage.tool_bytes + 100 &&
                projected->provider_history.size() == base->provider_history.size(),
            "projected tool outcomes must charge mandatory status, call IDs, payload separators, and enclosing history overhead");
}

i32 run_all() {
    test_resolution_order_scope_and_determinism();
    test_workspace_boundary();
    test_project_root_discovery();
    test_instruction_read_failure();
    test_instruction_read_is_strict();
    test_manifest_deduplication_and_redacted_description();
    test_bounded_selection_keeps_complete_recent_tasks();
    test_reported_usage_accounts_for_trailing_context();
    test_projected_tool_outcomes_charge_exact_envelope_overhead();
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
