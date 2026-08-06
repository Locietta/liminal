#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
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
        .then_calls([&](const std::filesystem::path &path) -> Result<std::optional<std::string>> {
            require(path == root / "AGENTS.md", "resolver did not read the root instruction first");
            return std::optional<std::string>{"root policy"};
        })
        .then_calls([&](const std::filesystem::path &path) -> Result<std::optional<std::string>> {
            require(path == root / "src" / "AGENTS.md", "resolver did not inspect the intermediate scope");
            return std::optional<std::string>{};
        })
        .then_calls([&](const std::filesystem::path &path) -> Result<std::optional<std::string>> {
            require(path == active / "AGENTS.md", "resolver did not read the narrow instruction last");
            return std::optional<std::string>{"module policy"};
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
    files.expect<context::ReadInstructionFileDispatch>().calls([](const std::filesystem::path &) -> Result<std::optional<std::string>> {
        return lighter::outcome_error(Error::config("permission denied"));
    });

    auto handle = files.handle();
    auto resolved = context::ProjectInstructionResolver{}.resolve("workspace", "workspace", handle);
    require(!resolved && resolved.error().detail == "permission denied", "resolver hid or replaced an instruction read failure");
    files.verify();
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
    session::Session session_log({.value = 7});
    session_log.append(session::UserMessage{.text = "OLD SECRET"});
    session::ContextCheckpoint checkpoint;
    checkpoint.items.push_back(session::ContextInput{.parts = {provider::TextPart{.text = "SUMMARY SECRET"}}});
    const auto checkpoint_id = session_log.append(std::move(checkpoint));
    session_log.append(session::UserMessage{.text = "CURRENT SECRET"});

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

i32 run_all() {
    test_resolution_order_scope_and_determinism();
    test_workspace_boundary();
    test_project_root_discovery();
    test_instruction_read_failure();
    test_manifest_deduplication_and_redacted_description();
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
