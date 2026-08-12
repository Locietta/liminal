#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <glaze/json.hpp>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/when.h>
#include <lighter/types.hpp>

#include <liminal/provider/common.h>
#include <liminal/tools/tools.h>

namespace {

using namespace lighter::types;
using namespace liminal;
void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

provider::ToolCall make_call(std::string id, std::string name, std::string_view input_json) {
    glz::generic input;
    auto parse_error = glz::read_json(input, input_json);
    require(!parse_error, "failed to create tool input");
    return {
        .id = std::move(id),
        .name = std::move(name),
        .input = std::move(input),
    };
}

Result<provider::ToolResult> execute(ToolSet &tools, provider::ToolCall call) {
    lighter::EventLoop loop;
    auto task = tools.execute(call);
    loop.schedule(task);
    loop.run();
    return task.result();
}

void test_tools_are_available_by_default() {
    ToolSet tools(std::filesystem::current_path() / "liminal");
    auto definitions = tools.definitions();
    require(definitions.size() == 6 && definitions[0].name == "read_file" && definitions[1].name == "apply_patch" &&
                definitions[2].name == "exec_command" && definitions[3].name == "write_stdin" &&
                definitions[4].kind == provider::ToolKind::WEB_SEARCH && definitions[5].kind == provider::ToolKind::WEB_FETCH,
            "default tools must include file reading, patch editing, interactive shell execution, and hosted web access");
    const auto &write_stdin = definitions[3];
    require(write_stdin.input_schema.properties.at("session_id").type == "integer" && write_stdin.input_schema.required.size() == 1 &&
                write_stdin.input_schema.required.front() == "session_id",
            "write_stdin must require the numeric session ID returned by exec_command");
    require(tools.execution_mode("read_file") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("exec_command") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("write_stdin") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("apply_patch") == ToolExecutionMode::EXCLUSIVE &&
                tools.execution_mode("unknown") == ToolExecutionMode::EXCLUSIVE,
            "built-in and unknown tools must expose conservative execution semantics");

    auto readme = execute(tools, make_call("read", "read_file", R"({"path":"../README.md"})"));
    require(readme.has_value() && !readme->is_error && readme->content.contains("Liminal"),
            "read_file must allow paths outside the working directory");

    auto command = execute(tools, make_call("command", "exec_command", R"({"cmd":"pwd"})"));
    require(command.has_value() && command->call_id == "command" && !command->is_error && command->content.contains("exit_code: 0") &&
                command->content.contains("output:\n"),
            "exec_command must execute a short command without an opt-in mode");
}

void test_tool_registry_dispatches_extensions() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "echo_extension", .description = "Test extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            co_return provider::ToolResult{.call_id = call.id, .content = "extended"};
        },
    });
    require(registered.has_value(), "tool registry rejected a valid extension");
    require(tools.execution_mode("echo_extension") == ToolExecutionMode::EXCLUSIVE,
            "extensions must default to exclusive execution until they opt into parallelism");

    auto result = execute(tools, make_call("extension", "echo_extension", R"({})"));
    require(result.has_value() && result->content == "extended", "tool registry did not dispatch an extension");

    auto duplicate = tools.register_tool({
        .definition = {.name = "echo_extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            co_return provider::ToolResult{.call_id = call.id};
        },
    });
    require(!duplicate && duplicate.error().detail.contains("duplicate"), "tool registry must reject duplicate names");
}

void test_tool_presentations_are_specific_and_bounded() {
    const auto read = make_call("read", "read_file", R"({"path":"docs/code-style-guide.md"})");
    ToolSet tools(std::filesystem::current_path());
    const auto read_presentation = tools.describe(read);
    require(read_presentation.description == "Read docs/code-style-guide.md" && read_presentation.command.empty(),
            "read_file presentation must name the requested path without pretending it is a shell command");
    const provider::ToolResult read_result{.call_id = "read", .content = "first\nsecond\n"};
    require(tools.summarize(read, read_result) == "2 lines · 13 bytes",
            "read_file completion must summarize line and byte counts without echoing file contents");

    const auto command = make_call("command", "exec_command", R"({"cmd":"pixi run build\npixi run test-unit"})");
    const auto presentation = tools.describe(command);
    require(presentation.description.empty() && presentation.command == "pixi run build\npixi run test-unit",
            "exec_command presentation must retain the exact multiline command as semantic data");
    const provider::ToolResult command_result{
        .call_id = "command",
        .content = "session_id: 2\nstatus: exited\nexit_code: 7\n\noutput:\nconfigured\nbuilt\none test failed\n",
        .is_error = true,
    };
    const auto summary = tools.summarize(command, command_result);
    require(summary.contains("exec session 2 · exit 7") && summary.contains("configured") && summary.contains("one test failed"),
            "exec_command completion must expose exec session state and a bounded output preview");
}

void test_read_file_is_bounded_and_regular() {
    constexpr usize k_file_limit = 128 * 1024;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-tools-" + std::to_string(nonce));
    std::filesystem::create_directories(directory / "folder");
    {
        std::ofstream output(directory / "large.txt", std::ios::binary);
        output << std::string(k_file_limit + 4096, 'x');
        require(static_cast<bool>(output), "failed to create the large read_file fixture");
    }

    ToolSet tools(directory);
    auto large = execute(tools, make_call("large", "read_file", R"({"path":"large.txt"})"));
    require(large.has_value() && !large->is_error && large->content.starts_with(std::string(k_file_limit, 'x')) &&
                large->content.ends_with("[truncated after 131072 bytes; next offset 131072]"),
            "read_file did not return a bounded prefix for a large file");

    {
        std::ofstream output(directory / "lines.txt", std::ios::binary);
        output << "one\r\ntwo\r\nthree\r\nfour\r\n";
    }
    auto lines = execute(tools, make_call("lines", "read_file", R"({"path":"lines.txt","line_start":2,"line_end":3})"));
    require(lines.has_value() && !lines->is_error && lines->content == "two\nthree\n",
            "read_file must support one-based inclusive line ranges and normalize CRLF");

    auto bytes = execute(tools, make_call("bytes", "read_file", R"({"path":"lines.txt","offset":5,"limit":3})"));
    require(bytes.has_value() && !bytes->is_error && bytes->content.starts_with("two") && bytes->content.contains("next offset 8"),
            "read_file must support resumable byte ranges");

    auto mixed = execute(tools, make_call("mixed", "read_file", R"({"path":"lines.txt","offset":0,"line_start":1})"));
    require(mixed.has_value() && mixed->is_error && mixed->content.contains("cannot be combined"),
            "read_file must reject ambiguous mixed ranges");

    auto empty = execute(tools, make_call("empty", "read_file", R"({"path":"lines.txt","limit":0})"));
    require(empty.has_value() && empty->is_error && empty->content.contains("at least 1"),
            "read_file must reject a zero byte limit that cannot advance a resumed read");

    auto folder = execute(tools, make_call("folder", "read_file", R"({"path":"folder"})"));
    require(folder.has_value() && folder->is_error && folder->content.contains("is not a regular file"),
            "read_file did not reject a directory");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

void test_apply_patch_operations_are_validated_before_writes() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-patch-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(directory / "source.txt", std::ios::binary);
        output << "alpha\r\nbeta\r\ngamma\r\n";
    }
    ToolSet tools(directory);
    auto changed = execute(
        tools,
        make_call(
            "patch", "apply_patch",
            R"json({"patch":"*** Begin Patch\n*** Add File: added.txt\n+created\n*** Update File: source.txt\n@@ missing context\n-beta\n+changed\n*** End Patch"})json"));
    require(changed.has_value() && changed->is_error && changed->content.contains("context not found"),
            "apply_patch must validate every file hunk before writing any operation");
    require(!std::filesystem::exists(directory / "added.txt"), "apply_patch wrote an earlier operation before validation completed");

    auto applied = execute(
        tools,
        make_call(
            "patch", "apply_patch",
            R"json({"patch":"*** Begin Patch\n*** Add File: added.txt\n+created\n*** Update File: source.txt\n@@ alpha\n-beta\n+changed\n*** End Patch"})json"));
    require(applied.has_value() && !applied->is_error && applied->content.contains("Added: added.txt") &&
                applied->content.contains("Updated: source.txt"),
            "apply_patch did not report added and updated files");

    std::ifstream source(directory / "source.txt", std::ios::binary);
    const std::string source_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(source_text == "alpha\r\nchanged\r\ngamma\r\n", "apply_patch must preserve the updated file's CRLF convention");

    {
        std::ofstream output(directory / "fuzzy.txt", std::ios::binary);
        output << "start\n  repeated  \nold\n  repeated  \nold\n";
    }
    auto fuzzy = execute(
        tools,
        make_call(
            "fuzzy", "apply_patch",
            R"json({"patch":"*** Begin Patch\n*** Update File: fuzzy.txt\n@@\n repeated\n-old\n+last\n*** End of File\n*** End Patch"})json"));
    require(fuzzy.has_value() && !fuzzy->is_error, "apply_patch must tolerate surrounding whitespace in hunk matches");
    std::ifstream fuzzy_file(directory / "fuzzy.txt", std::ios::binary);
    const std::string fuzzy_text((std::istreambuf_iterator<char>(fuzzy_file)), std::istreambuf_iterator<char>());
    require(fuzzy_text == "start\n  repeated  \nold\n  repeated  \nlast\n",
            "apply_patch must preserve context whitespace and honor end-of-file matching");

    auto moved = execute(
        tools, make_call("move", "apply_patch",
                         R"json({"patch":"*** Begin Patch\n*** Update File: added.txt\n*** Move to: moved.txt\n*** End Patch"})json"));
    require(moved.has_value() && !moved->is_error && !std::filesystem::exists(directory / "added.txt") &&
                std::filesystem::exists(directory / "moved.txt"),
            "apply_patch must support move-only updates");

    auto removed = execute(
        tools, make_call("delete", "apply_patch", R"json({"patch":"*** Begin Patch\n*** Delete File: moved.txt\n*** End Patch"})json"));
    require(removed.has_value() && !removed->is_error && !std::filesystem::exists(directory / "moved.txt"),
            "apply_patch must delete existing files");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

lighter::Task<> exercise_shell_task_interaction(ToolSet &tools) {
#ifdef _WIN32
    constexpr std::string_view command =
        R"json({"cmd":"$first = [Console]::In.ReadLine(); Start-Sleep -Milliseconds 100; Write-Output ('first:' + $first); $second = [Console]::In.ReadLine(); Start-Sleep -Milliseconds 100; Write-Output ('second:' + $second)","yield_time_ms":25})json";
#else
    constexpr std::string_view command =
        R"({"cmd":"IFS= read -r first; sleep 0.1; echo first:$first; IFS= read -r second; sleep 0.1; echo second:$second","yield_time_ms":25})";
#endif
    auto started = co_await tools.execute(make_call("start", "exec_command", command));
    require(started.has_value() && !started->is_error && started->content.contains("session_id: 1") &&
                started->content.contains("status: running"),
            "exec_command must return a session ID for a command awaiting input");

    auto interacted = co_await lighter::WhenAll(
        tools.execute(make_call("first", "write_stdin", R"({"session_id":1,"chars":"one\n","yield_time_ms":3000})")),
        tools.execute(make_call("second", "write_stdin", R"({"session_id":1,"chars":"two\n","yield_time_ms":3000})")));
    require(interacted.has_value(), "concurrent writes to one exec session failed");
    const auto &[first, second] = *interacted;
    require(first.content.contains("first:one") && !first.content.contains("second:two") && second.content.contains("second:two"),
            "writes to one exec session must remain ordered and drain distinct output");
    auto finished = co_await tools.execute(make_call("finish", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})"));
    require(finished && !finished->is_error && finished->content.contains("status: exited") && finished->content.contains("exit_code: 0"),
            "the ordered exec session writes did not run through completion");
}

void test_shell_task_interaction() {
    lighter::EventLoop loop;
    ToolSet tools(std::filesystem::current_path());
    auto task = exercise_shell_task_interaction(tools);
    loop.schedule(task);
    loop.run();
    std::ignore = task.result();
}

std::string exec_command_input(std::string_view command, const std::filesystem::path &working_directory) {
    return "{\"cmd\":\"" + std::string(command) + "\",\"workdir\":\"" + working_directory.generic_string() + "\",\"yield_time_ms\":25}";
}

lighter::Task<> exercise_distinct_shell_task_interactions(ToolSet &tools, const std::filesystem::path &working_directory) {
#ifdef _WIN32
    constexpr std::string_view first_command =
        R"($null = [Console]::In.ReadLine(); Set-Content -Path a.ready -Value ready -NoNewline; for ($i = 0; $i -lt 500 -and -not (Test-Path b.ready); ++$i) { Start-Sleep -Milliseconds 10 }; if (-not (Test-Path b.ready)) { exit 2 }; Write-Output task-a)";
    constexpr std::string_view second_command =
        R"($null = [Console]::In.ReadLine(); Set-Content -Path b.ready -Value ready -NoNewline; for ($i = 0; $i -lt 500 -and -not (Test-Path a.ready); ++$i) { Start-Sleep -Milliseconds 10 }; if (-not (Test-Path a.ready)) { exit 2 }; Write-Output task-b)";
#else
    constexpr std::string_view first_command =
        "IFS= read -r line; : > a.ready; i=0; while [ ! -f b.ready ] && [ $i -lt 500 ]; do sleep 0.01; i=$((i+1)); done; [ -f "
        "b.ready ] || exit 2; echo task-a";
    constexpr std::string_view second_command =
        "IFS= read -r line; : > b.ready; i=0; while [ ! -f a.ready ] && [ $i -lt 500 ]; do sleep 0.01; i=$((i+1)); done; [ -f "
        "a.ready ] || exit 2; echo task-b";
#endif
    auto first_started = co_await tools.execute(make_call("start-a", "exec_command", exec_command_input(first_command, working_directory)));
    auto second_started =
        co_await tools.execute(make_call("start-b", "exec_command", exec_command_input(second_command, working_directory)));
    require(first_started && second_started && first_started->content.contains("session_id: 1") &&
                second_started->content.contains("session_id: 2"),
            "failed to start distinct exec sessions for concurrent writes");

    auto interacted = co_await lighter::WhenAll(
        tools.execute(make_call("write-a", "write_stdin", R"({"session_id":1,"chars":"go\n","yield_time_ms":2000})")),
        tools.execute(make_call("write-b", "write_stdin", R"({"session_id":2,"chars":"go\n","yield_time_ms":2000})")));
    require(interacted.has_value(), "writes to distinct exec sessions failed");
    const auto &[first, second] = *interacted;
    require(first.content.contains("task-a") && second.content.contains("task-b"), "writes to distinct exec sessions blocked each other");

    auto finished =
        co_await lighter::WhenAll(tools.execute(make_call("finish-a", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})")),
                                  tools.execute(make_call("finish-b", "write_stdin", R"({"session_id":2,"yield_time_ms":3000})")));
    require(finished.has_value(), "failed to finish distinct exec sessions");
    const auto &[first_finished, second_finished] = *finished;
    require(first_finished.content.contains("status: exited") && second_finished.content.contains("status: exited"),
            "distinct exec sessions did not run through completion");
}

void test_distinct_shell_tasks_interact_concurrently() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-shell-tasks-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);

    lighter::EventLoop loop;
    ToolSet tools(std::filesystem::current_path());
    auto task = exercise_distinct_shell_task_interactions(tools, directory);
    loop.schedule(task);
    loop.run();
    std::ignore = task.result();

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

void test_nonzero_command_is_an_error_result() {
    ToolSet tools(std::filesystem::current_path());
#ifdef _WIN32
    auto call = make_call("nonzero", "exec_command", R"({"cmd":"Write-Error 'failed'; exit 7"})");
#else
    auto call = make_call("nonzero", "exec_command", R"({"cmd":"printf 'failed\\n' >&2; exit 7"})");
#endif
    auto outcome = execute(tools, std::move(call));
    require(outcome.has_value() && outcome->is_error && outcome->content.contains("exit_code: 7"),
            "a nonzero shell exit must remain a tool result while entering the failed UI state");
}

i32 run_all() {
    test_tools_are_available_by_default();
    test_tool_registry_dispatches_extensions();
    test_tool_presentations_are_specific_and_bounded();
    test_read_file_is_bounded_and_regular();
    test_apply_patch_operations_are_validated_before_writes();
    test_shell_task_interaction();
    test_distinct_shell_tasks_interact_concurrently();
    test_nonzero_command_is_an_error_result();
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
