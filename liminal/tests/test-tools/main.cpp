#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <glaze/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

#include <lighter/async/async.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/when.h>
#include <lighter/encoding/utf8.h>
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

constexpr ToolOutputGrant k_test_grant{.receipt_bytes = 64 * 1024, .payload_bytes = k_max_tool_payload_bytes};

Result<ToolOutcome> execute(ToolSet &tools, provider::ToolCall call, ToolOutputGrant grant = k_test_grant) {
    lighter::EventLoop loop;
    auto task = tools.execute(std::move(call), grant);
    loop.schedule(task);
    loop.run();
    return task.result();
}

Result<ToolOutcome> execute_prepared(PreparedToolCall call, ToolOutputGrant grant = k_test_grant) {
    lighter::EventLoop loop;
    auto task = call.execute(grant);
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();
    if (outcome) finalize_tool_outcome(*outcome, grant);
    return outcome;
}

void test_tools_are_available_by_default() {
    ToolSet tools(std::filesystem::current_path() / "liminal");
    auto definitions = tools.definitions();
    require(definitions.size() == 7 && definitions[0].name == "read_file" && definitions[1].name == "read_file_bytes" &&
                definitions[2].name == "apply_patch" && definitions[3].name == "exec_command" && definitions[4].name == "write_stdin" &&
                definitions[5].kind == provider::ToolKind::WEB_SEARCH && definitions[6].kind == provider::ToolKind::WEB_FETCH,
            "default tools must include file reading, patch editing, interactive shell execution, and hosted web access");
    const auto &write_stdin = definitions[4];
    require(write_stdin.input_schema.properties.at("session_id").type == "integer" && write_stdin.input_schema.required.size() == 1 &&
                write_stdin.input_schema.required.front() == "session_id",
            "write_stdin must require the numeric session ID returned by exec_command");
    require(definitions[0].input_schema.properties.contains("offset") && definitions[0].input_schema.properties.contains("limit") &&
                !definitions[0].input_schema.properties.contains("byte_offset") &&
                definitions[1].input_schema.properties.contains("byte_offset") &&
                !definitions[1].input_schema.properties.contains("offset"),
            "line and byte ranges must be exposed by separate file reading tools");
    require(tools.execution_mode("read_file") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("read_file_bytes") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("exec_command") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("write_stdin") == ToolExecutionMode::PARALLEL &&
                tools.execution_mode("apply_patch") == ToolExecutionMode::EXCLUSIVE &&
                tools.execution_mode("unknown") == ToolExecutionMode::EXCLUSIVE,
            "built-in and unknown tools must expose conservative execution semantics");

    auto readme = execute(tools, make_call("read", "read_file", R"({"path":"../README.md"})"));
    require(readme.has_value() && !tool_outcome_is_error(readme->kind) && readme->payload.contains("Liminal"),
            "read_file must allow paths outside the working directory");

    auto command = execute(tools, make_call("command", "exec_command", R"({"cmd":"pwd"})"));
    require(command.has_value() && command->call_id == "command" && !tool_outcome_is_error(command->kind) &&
                command->receipt.contains("exit_code: 0") && !command->payload.empty(),
            "exec_command must execute a short command without an opt-in mode");
}

void test_tool_registry_dispatches_extensions() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "echo_extension", .description = "Test extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id, .payload = "extended"};
        },
    });
    require(registered.has_value(), "tool registry rejected a valid extension");
    require(tools.execution_mode("echo_extension") == ToolExecutionMode::EXCLUSIVE,
            "extensions must default to exclusive execution until they opt into parallelism");

    auto result = execute(tools, make_call("extension", "echo_extension", R"({})"));
    require(result.has_value() && result->payload == "extended", "tool registry did not dispatch an extension");

    auto empty_prepared_registered = tools.register_tool({
        .definition = {.name = "empty_prepared_extension"},
        .prepare = [](const ToolSet &, const provider::ToolCall &) -> Result<PreparedToolExecution> {
            return PreparedToolExecution{.receipt_bytes = 32};
        },
    });
    require(empty_prepared_registered.has_value(), "tool registry rejected the empty prepared-executor fixture");
    auto empty_prepared = tools.prepare(make_call("empty-prepared", "empty_prepared_extension", R"({})"));
    require(!empty_prepared && empty_prepared.error().detail.contains("prepared no executor"),
            "ToolSet admitted a prepared invocation without an executor");

    auto bounded_registered = tools.register_tool({
        .definition = {.name = "large_extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id, .payload = std::string(128 * 1024, 'x')};
        },
    });
    require(bounded_registered.has_value(), "tool registry rejected the bounded-output test extension");
    auto bounded = execute(tools, make_call("bounded", "large_extension", R"({})"), {.receipt_bytes = 128, .payload_bytes = 128});
    require(bounded.has_value() && !tool_outcome_is_error(bounded->kind) && bounded->payload.size() <= 128 && bounded->payload_truncated,
            "ToolSet must bound extension payload without changing success semantics");
    auto ceiling = execute(tools, make_call("ceiling", "large_extension", R"({})"), {.receipt_bytes = 128, .payload_bytes = 128 * 1024});
    require(ceiling.has_value() && !tool_outcome_is_error(ceiling->kind) && ceiling->payload.size() == k_max_tool_payload_bytes,
            "ToolSet must enforce the absolute local-tool output ceiling");

    auto invalid_registered = tools.register_tool({
        .definition = {.name = "invalid_utf8_extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id, .payload = std::string("\xf0\x9f", 2)};
        },
    });
    require(invalid_registered.has_value(), "failed to register invalid UTF-8 extension fixture");
    auto sanitized = execute(tools, make_call("sanitize", "invalid_utf8_extension", R"({})"));
    require(sanitized && lighter::encoding::utf8::is_valid(sanitized->payload),
            "ToolSet must sanitize every successful local result even when it is under budget");

    auto duplicate = tools.register_tool({
        .definition = {.name = "echo_extension"},
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id};
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
    const ToolOutcome read_result{.call_id = "read", .payload = "first\nsecond\n"};
    require(tools.summarize(read, read_result) == "2 lines · 13 bytes",
            "read_file completion must summarize line and byte counts without echoing file contents");

    const auto command = make_call("command", "exec_command", R"({"cmd":"pixi run build\npixi run test-unit"})");
    const auto presentation = tools.describe(command);
    require(presentation.description.empty() && presentation.command == "pixi run build\npixi run test-unit",
            "exec_command presentation must retain the exact multiline command as semantic data");
    const ToolOutcome command_result{
        .call_id = "command",
        .kind = ToolOutcomeKind::FAILED,
        .receipt = "session_id: 2\nstatus: exited\nexit_code: 7",
        .payload = "configured\nbuilt\none test failed\n",
    };
    const auto summary = tools.summarize(command, command_result);
    require(summary.contains("exec session 2 · exit 7") && summary.contains("configured") && summary.contains("one test failed"),
            "exec_command completion must expose exec session state and a bounded output preview");
}

void test_read_file_is_bounded_and_regular() {
    constexpr usize k_read_payload_limit = k_max_tool_payload_bytes;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-tools-" + std::to_string(nonce));
    std::filesystem::create_directories(directory / "folder");
    {
        std::ofstream output(directory / "large.txt", std::ios::binary);
        output << std::string(2 * 1024 * 1024, 'x');
        require(static_cast<bool>(output), "failed to create the large read_file fixture");
    }

    ToolSet tools(directory);
    auto large = execute(tools, make_call("large", "read_file_bytes", R"({"path":"large.txt"})"));
    require(large.has_value() && !tool_outcome_is_error(large->kind) && large->payload == std::string(k_read_payload_limit, 'x') &&
                large->receipt.contains("next byte_offset 65536"),
            "read_file_bytes did not return a bounded prefix for a large file");

    {
        std::ofstream output(directory / "lines.txt", std::ios::binary);
        output << "one\r\ntwo\r\nthree\r\nfour\r\n";
    }
    auto whole = execute(tools, make_call("whole", "read_file", R"({"path":"lines.txt"})"));
    require(whole.has_value() && !tool_outcome_is_error(whole->kind) && whole->payload == "one\ntwo\nthree\nfour\n",
            "read_file must use normalized line-oriented reads by default");

    auto lines = execute(tools, make_call("lines", "read_file", R"({"path":"lines.txt","offset":2,"limit":2})"));
    require(lines.has_value() && !tool_outcome_is_error(lines->kind) && lines->payload == "two\nthree\n" &&
                lines->receipt.contains("continue with offset 4"),
            "read_file must support conventional one-based offset and line-count ranges with resumable output");

    auto bytes = execute(tools, make_call("bytes", "read_file_bytes", R"({"path":"lines.txt","byte_offset":5,"byte_count":3})"));
    require(bytes.has_value() && !tool_outcome_is_error(bytes->kind) && bytes->payload.starts_with("two") &&
                bytes->receipt.contains("next byte_offset 8"),
            "read_file_bytes must support resumable byte ranges");

    {
        std::ofstream output(directory / "utf8.txt", std::ios::binary);
        output << "A\xf0\x9f\x98\x80Z";
    }
    auto aligned = execute(tools, make_call("aligned", "read_file_bytes", R"({"path":"utf8.txt","byte_count":3})"));
    require(aligned && !tool_outcome_is_error(aligned->kind) && aligned->payload == "A" &&
                aligned->receipt.contains("truncated after 1 bytes") && lighter::encoding::utf8::is_valid(aligned->payload),
            "read_file_bytes must end chunks at a complete UTF-8 prefix");
    auto resumed = execute(tools, make_call("resumed", "read_file_bytes", R"({"path":"utf8.txt","byte_offset":1,"byte_count":4})"));
    require(resumed && !tool_outcome_is_error(resumed->kind) && resumed->payload.starts_with("\xf0\x9f\x98\x80") &&
                resumed->receipt.contains("next byte_offset 5") && lighter::encoding::utf8::is_valid(resumed->payload),
            "read_file_bytes must resume on a UTF-8 boundary");
    auto misaligned = execute(tools, make_call("misaligned", "read_file_bytes", R"({"path":"utf8.txt","byte_offset":2,"byte_count":3})"));
    require(misaligned && tool_outcome_is_error(misaligned->kind) && misaligned->payload.contains("falls inside a UTF-8 code point"),
            "read_file_bytes must reject a starting offset inside a UTF-8 code point");

    auto obsolete_range = tools.prepare(make_call("old-range", "read_file", R"({"path":"lines.txt","line_start":2})"));
    require(!obsolete_range, "read_file must reject obsolete line range parameter names");

    auto empty = execute(tools, make_call("empty", "read_file_bytes", R"({"path":"lines.txt","byte_count":0})"));
    require(empty.has_error() && empty.error().message().contains("at least 1"),
            "read_file_bytes must reject a byte count that cannot advance a resumed read");

    auto missing_path = tools.prepare(make_call("missing-path", "read_file", R"({})"));
    require(!missing_path && missing_path.error().message().contains("path must not be empty"),
            "read_file must reject a missing path before dispatch");

    auto zero_offset = tools.prepare(make_call("zero-offset", "read_file", R"({"path":"lines.txt","offset":0})"));
    require(!zero_offset && zero_offset.error().message().contains("offset must be at least 1"),
            "read_file must reject a zero line offset before dispatch");

    auto zero_limit = tools.prepare(make_call("zero-limit", "read_file", R"({"path":"lines.txt","limit":0})"));
    require(!zero_limit && zero_limit.error().message().contains("limit must be at least 1"),
            "read_file must reject a zero line limit before dispatch");

    auto oversized_line = execute(tools, make_call("oversized-line", "read_file", R"({"path":"large.txt"})"));
    require(oversized_line.has_value() && tool_outcome_is_error(oversized_line->kind) &&
                oversized_line->payload.contains("use read_file_bytes") && oversized_line->payload.contains("byte_offset 0"),
            "read_file must redirect oversized generated lines to read_file_bytes without a looping line continuation");

    {
        std::ofstream output(directory / "markers.txt", std::ios::binary);
        output << "Error: this is just log text\n... [truncated before line 9; continue with offset 9]\n" << std::string(600, 'y') << "\n";
    }
    auto markers = execute(tools, make_call("markers", "read_file", R"({"path":"markers.txt"})"));
    require(markers.has_value() && !tool_outcome_is_error(markers->kind) && markers->receipt.empty() &&
                markers->payload.starts_with("Error: this is just log text\n... [truncated before line 9") &&
                markers->payload.ends_with(std::string(600, 'y') + "\n"),
            "file content that looks like a read marker or error must be returned verbatim as payload");
    auto marker_bytes = execute(tools, make_call("marker-bytes", "read_file_bytes", R"({"path":"markers.txt","byte_count":80})"));
    require(marker_bytes.has_value() && !tool_outcome_is_error(marker_bytes->kind) && marker_bytes->payload.size() == 80 &&
                marker_bytes->receipt.contains("next byte_offset 80"),
            "read_file_bytes must not confuse marker-like content with its own continuation note");

    auto folder = execute(tools, make_call("folder", "read_file", R"({"path":"folder"})"));
    require(folder.has_value() && tool_outcome_is_error(folder->kind) && folder->payload.contains("is not a regular file"),
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
    require(changed.has_value() && tool_outcome_is_error(changed->kind) && changed->payload.contains("context not found"),
            "apply_patch must validate every file hunk before writing any operation");
    require(!std::filesystem::exists(directory / "added.txt"), "apply_patch wrote an earlier operation before validation completed");

    auto no_capacity = tools.prepare(make_call(
        "capacity", "apply_patch",
        R"json({"patch":"*** Begin Patch\n*** Add File: capacity-receipt-that-cannot-fit-within-the-committed-output-budget.txt\n+must not be written\n*** End Patch"})json"));
    require(no_capacity.has_value() && no_capacity->receipt_bytes > 64,
            "apply_patch preparation must expose its exact success-receipt requirement before writing");
    require(!std::filesystem::exists(directory / "capacity-receipt-that-cannot-fit-within-the-committed-output-budget.txt"),
            "apply_patch mutated files without capacity for its success receipt");

    auto applied = execute(
        tools,
        make_call(
            "patch", "apply_patch",
            R"json({"patch":"*** Begin Patch\n*** Add File: added.txt\n+created\n*** Update File: source.txt\n@@ alpha\n-beta\n+changed\n*** End Patch"})json"));
    require(applied.has_value() && !tool_outcome_is_error(applied->kind) && applied->receipt.contains("Added: added.txt") &&
                applied->receipt.contains("Updated: source.txt"),
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
    require(fuzzy.has_value() && !tool_outcome_is_error(fuzzy->kind), "apply_patch must tolerate surrounding whitespace in hunk matches");
    std::ifstream fuzzy_file(directory / "fuzzy.txt", std::ios::binary);
    const std::string fuzzy_text((std::istreambuf_iterator<char>(fuzzy_file)), std::istreambuf_iterator<char>());
    require(fuzzy_text == "start\n  repeated  \nold\n  repeated  \nlast\n",
            "apply_patch must preserve context whitespace and honor end-of-file matching");

    auto moved = execute(
        tools, make_call("move", "apply_patch",
                         R"json({"patch":"*** Begin Patch\n*** Update File: added.txt\n*** Move to: moved.txt\n*** End Patch"})json"));
    require(moved.has_value() && !tool_outcome_is_error(moved->kind) && !std::filesystem::exists(directory / "added.txt") &&
                std::filesystem::exists(directory / "moved.txt"),
            "apply_patch must support move-only updates");

    auto removed = execute(
        tools, make_call("delete", "apply_patch", R"json({"patch":"*** Begin Patch\n*** Delete File: moved.txt\n*** End Patch"})json"));
    require(removed.has_value() && !tool_outcome_is_error(removed->kind) && !std::filesystem::exists(directory / "moved.txt"),
            "apply_patch must delete existing files");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

void test_apply_patch_resolves_files_when_executed() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-patch-order-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(directory / "source.txt", std::ios::binary);
        output << "alpha\nbeta\n";
    }

    ToolSet tools(directory);
    auto first = tools.prepare(make_call(
        "first", "apply_patch", R"json({"patch":"*** Begin Patch\n*** Update File: source.txt\n@@\n-alpha\n+first\n*** End Patch"})json"));
    auto second = tools.prepare(make_call(
        "second", "apply_patch", R"json({"patch":"*** Begin Patch\n*** Update File: source.txt\n@@\n-beta\n+second\n*** End Patch"})json"));
    require(first.has_value() && second.has_value(), "apply_patch calls were not prepared from parsed operations");

    auto first_outcome = execute_prepared(*std::move(first));
    auto second_outcome = execute_prepared(*std::move(second));
    require(first_outcome && second_outcome && !tool_outcome_is_error(first_outcome->kind) && !tool_outcome_is_error(second_outcome->kind),
            "sequential prepared patches did not execute successfully");

    std::ifstream source(directory / "source.txt", std::ios::binary);
    const std::string source_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(source_text == "first\nsecond\n", "a prepared patch overwrote changes committed by an earlier exclusive call");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

void test_apply_patch_insert_only_hunk_lands_after_its_anchor() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-patch-insert-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(directory / "source.txt", std::ios::binary);
        output << "class Foo\nbody\nclass Bar\nbody\n";
    }

    ToolSet tools(directory);
    auto anchored = tools.prepare(
        make_call("anchored", "apply_patch",
                  R"json({"patch":"*** Begin Patch\n*** Update File: source.txt\n@@ class Foo\n+inserted\n*** End Patch"})json"));
    require(anchored.has_value(), "an insert-only hunk with a context anchor must prepare");
    auto anchored_outcome = execute_prepared(*std::move(anchored));
    require(anchored_outcome && !tool_outcome_is_error(anchored_outcome->kind), "an anchored insert-only hunk must apply");

    auto at_end = tools.prepare(
        make_call("at-end", "apply_patch",
                  R"json({"patch":"*** Begin Patch\n*** Update File: source.txt\n@@\n+trailing\n*** End of File\n*** End Patch"})json"));
    require(at_end.has_value(), "an insert-only hunk at end of file must prepare");
    auto at_end_outcome = execute_prepared(*std::move(at_end));
    require(at_end_outcome && !tool_outcome_is_error(at_end_outcome->kind), "an end-of-file insert-only hunk must apply");

    std::ifstream source(directory / "source.txt", std::ios::binary);
    const std::string source_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(source_text == "class Foo\ninserted\nbody\nclass Bar\nbody\ntrailing\n",
            "insert-only hunks must land directly after their anchor, or at end of file only when requested: got " + source_text);

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

struct ExecObservations {
    void append(const ToolOutcome &result) {
        if (!diagnostics.empty()) diagnostics += '\n';
        diagnostics += render_tool_outcome(result);
        output += result.payload;
        if (result.receipt.contains("status: exited")) {
            exited = true;
            exit_zero = result.receipt.contains("exit_code: 0") && !tool_outcome_is_error(result.kind);
        }
    }

    std::string diagnostics;
    std::string output;
    bool exited = false;
    bool exit_zero = false;
};

lighter::Task<> exercise_shell_task_interaction(ToolSet &tools) {
#ifdef _WIN32
    constexpr std::string_view command =
        R"json({"cmd":"$first = [Console]::In.ReadLine(); Write-Output ('first:' + $first); $second = [Console]::In.ReadLine(); Write-Output ('second:' + $second)","yield_time_ms":25})json";
#else
    constexpr std::string_view command =
        R"({"cmd":"IFS= read -r first; echo first:$first; IFS= read -r second; echo second:$second","yield_time_ms":25})";
#endif
    auto started = co_await tools.execute(make_call("start", "exec_command", command), k_test_grant);
    require(started.has_value() && !tool_outcome_is_error(started->kind) && started->receipt.contains("session_id: 1") &&
                started->receipt.contains("status: running"),
            "exec_command must return a session ID for a command awaiting input");

    auto interacted = co_await lighter::WhenAll(
        tools.execute(make_call("first", "write_stdin", R"({"session_id":1,"chars":"one\n","yield_time_ms":3000})"), k_test_grant),
        tools.execute(make_call("second", "write_stdin", R"({"session_id":1,"chars":"two\n","yield_time_ms":3000})"), k_test_grant));
    require(interacted.has_value(), "concurrent writes to one exec session failed");
    const auto &[first, second] = *interacted;
    ExecObservations observations;
    observations.append(first);
    observations.append(second);
    for (usize attempt = 0; attempt < 8 && !observations.exited; ++attempt) {
        auto polled = co_await tools.execute(make_call("finish", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})"), k_test_grant);
        require(polled.has_value(), "polling the ordered exec session failed");
        observations.append(*polled);
    }
    const auto first_output = observations.output.find("first:one");
    const auto second_output = observations.output.find("second:two");
    require(first_output != std::string::npos && second_output != std::string::npos && first_output < second_output,
            "writes to one exec session were not observed in call order: " + observations.diagnostics);
    require(observations.exited && observations.exit_zero,
            "the ordered exec session did not run through completion: " + observations.diagnostics);
}

/// Waits for a process that is not our child to disappear. Windows can wait
/// on the process object; POSIX has no handle for a non-child, so poll the
/// pid with a bounded number of short sleeps.
lighter::Task<bool> process_terminated(i64 pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) co_return true;
    const auto waited = WaitForSingleObject(handle, 5000);
    CloseHandle(handle);
    co_return waited == WAIT_OBJECT_0;
#else
    for (usize attempt = 0; attempt < 200; ++attempt) {
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) co_return true;
        co_await lighter::sleep(std::chrono::milliseconds(25));
    }
    co_return false;
#endif
}

lighter::Task<> poll_until_exited(ToolSet &tools, ExecObservations &observations, std::string_view context) {
    for (usize attempt = 0; attempt < 8 && !observations.exited; ++attempt) {
        auto polled = co_await tools.execute(make_call("poll", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})"), k_test_grant);
        require(polled.has_value(), std::string(context) + ": polling the exec session failed");
        observations.append(*polled);
    }
}

lighter::Task<> exercise_shell_session_eof(ToolSet &tools) {
#ifdef _WIN32
    constexpr std::string_view command =
        R"json({"cmd":"$all = [Console]::In.ReadToEnd(); Write-Output ('got:' + $all.Trim())","yield_time_ms":25})json";
#else
    constexpr std::string_view command = R"json({"cmd":"all=$(cat); echo got:$all","yield_time_ms":25})json";
#endif
    auto started = co_await tools.execute(make_call("start", "exec_command", command), k_test_grant);
    require(started.has_value() && !tool_outcome_is_error(started->kind) && started->receipt.contains("status: running"),
            "a command reading to end of input must stay running until stdin closes");

    ExecObservations observations;
    auto closed = co_await tools.execute(
        make_call("eof", "write_stdin", R"({"session_id":1,"chars":"abc\n","eof":true,"yield_time_ms":3000})"), k_test_grant);
    require(closed.has_value(), "closing exec session stdin failed");
    observations.append(*closed);
    co_await poll_until_exited(tools, observations, "eof");
    require(observations.output.contains("got:abc") && observations.exited && observations.exit_zero,
            "closing stdin must let a program reading to end of input finish: " + observations.diagnostics);
}

lighter::Task<> exercise_shell_session_kill(ToolSet &tools) {
#ifdef _WIN32
    constexpr std::string_view command =
        R"json({"cmd":"$p = Start-Process -FilePath pwsh -ArgumentList '-NoProfile','-NonInteractive','-Command','Start-Sleep 300' -PassThru -WindowStyle Hidden; Write-Output ('child:' + $p.Id); Start-Sleep 300","yield_time_ms":25})json";
#else
    constexpr std::string_view command = R"json({"cmd":"sleep 300 & echo child:$!; wait","yield_time_ms":25})json";
#endif
    auto started = co_await tools.execute(make_call("start", "exec_command", command), k_test_grant);
    require(started.has_value() && !tool_outcome_is_error(started->kind) && started->receipt.contains("status: running"),
            "the long-running command must report a running session");

    ExecObservations observations;
    observations.append(*started);
    for (usize attempt = 0; attempt < 8 && !observations.output.contains("child:"); ++attempt) {
        auto polled = co_await tools.execute(make_call("poll", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})"), k_test_grant);
        require(polled.has_value(), "polling for the grandchild pid failed");
        observations.append(*polled);
    }
    const auto marker = observations.output.find("child:");
    require(marker != std::string::npos, "the shell never reported its grandchild pid: " + observations.diagnostics);
    const auto pid = std::stoll(observations.output.substr(marker + 6));
    require(pid > 0, "the reported grandchild pid is not usable");

    auto killed =
        co_await tools.execute(make_call("kill", "write_stdin", R"({"session_id":1,"kill":true,"yield_time_ms":5000})"), k_test_grant);
    require(killed.has_value(), "killing the exec session failed");
    observations.append(*killed);
    co_await poll_until_exited(tools, observations, "kill");
    require(observations.exited && killed->receipt.contains("killed: true"),
            "a killed session must report that it exited because it was killed: " + observations.diagnostics);
    require(co_await process_terminated(pid), "killing the session must also terminate the process it started");

    auto late = co_await tools.execute(make_call("late", "write_stdin", R"({"session_id":1,"chars":"x"})"), k_test_grant);
    require(late.has_error(), "writing to a killed session must be rejected");
}

void test_shell_session_eof_and_kill() {
    {
        lighter::EventLoop loop;
        ToolSet tools(std::filesystem::current_path());
        auto task = exercise_shell_session_eof(tools);
        loop.schedule(task);
        loop.run();
        std::ignore = task.result();
    }
    {
        lighter::EventLoop loop;
        ToolSet tools(std::filesystem::current_path());
        auto task = exercise_shell_session_kill(tools);
        loop.schedule(task);
        loop.run();
        std::ignore = task.result();
    }
}

void test_shell_task_interaction() {
    lighter::EventLoop loop;
    ToolSet tools(std::filesystem::current_path());
    auto task = exercise_shell_task_interaction(tools);
    loop.schedule(task);
    loop.run();
    std::ignore = task.result();
}

lighter::Task<> exercise_shell_output_receipt(ToolSet &tools) {
#ifdef _WIN32
    constexpr std::string_view command = R"json({"cmd":"Write-Output ('x' * 96)","yield_time_ms":30000})json";
#else
    constexpr std::string_view command = R"json({"cmd":"printf '%096d' 0","yield_time_ms":30000})json";
#endif
    constexpr ToolOutputGrant small_grant{.receipt_bytes = 512, .payload_bytes = 32};
    auto result = co_await tools.execute(make_call("start", "exec_command", command), small_grant);
    require(result && result->receipt.contains("session_id: 1") && result->receipt.contains("output_remaining_bytes:") &&
                result->receipt.contains("next_action: poll_with_write_stdin") && result->payload.size() <= small_grant.payload_bytes,
            "bounded shell output did not preserve its resumable control receipt");
    auto output = result->payload;
    for (usize attempt = 0; attempt < 8 && result->receipt.contains("output_remaining_bytes:"); ++attempt) {
        result = co_await tools.execute(make_call("poll", "write_stdin", R"({"session_id":1,"yield_time_ms":0})"), small_grant);
        require(result && result->payload.size() <= small_grant.payload_bytes,
                "polling bounded shell output exceeded its immutable payload grant");
        output += result->payload;
    }
    require(result && !result->receipt.contains("output_remaining_bytes:") && output.size() >= 96,
            "bounded shell output could not be resumed through its durable session receipt");
}

void test_shell_output_receipt_is_resumable() {
    lighter::EventLoop loop;
    ToolSet tools(std::filesystem::current_path());
    auto task = exercise_shell_output_receipt(tools);
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
    auto first_started =
        co_await tools.execute(make_call("start-a", "exec_command", exec_command_input(first_command, working_directory)), k_test_grant);
    auto second_started =
        co_await tools.execute(make_call("start-b", "exec_command", exec_command_input(second_command, working_directory)), k_test_grant);
    require(first_started && second_started && first_started->receipt.contains("session_id: 1") &&
                second_started->receipt.contains("session_id: 2"),
            "failed to start distinct exec sessions for concurrent writes");

    auto interacted = co_await lighter::WhenAll(
        tools.execute(make_call("write-a", "write_stdin", R"({"session_id":1,"chars":"go\n","yield_time_ms":2000})"), k_test_grant),
        tools.execute(make_call("write-b", "write_stdin", R"({"session_id":2,"chars":"go\n","yield_time_ms":2000})"), k_test_grant));
    require(interacted.has_value(), "writes to distinct exec sessions failed");
    const auto &[first, second] = *interacted;
    ExecObservations first_observations;
    ExecObservations second_observations;
    first_observations.append(first);
    second_observations.append(second);
    for (usize attempt = 0; attempt < 8 && (!first_observations.exited || !second_observations.exited); ++attempt) {
        auto finished = co_await lighter::WhenAll(
            tools.execute(make_call("finish-a", "write_stdin", R"({"session_id":1,"yield_time_ms":3000})"), k_test_grant),
            tools.execute(make_call("finish-b", "write_stdin", R"({"session_id":2,"yield_time_ms":3000})"), k_test_grant));
        require(finished.has_value(), "failed to poll distinct exec sessions");
        const auto &[first_finished, second_finished] = *finished;
        first_observations.append(first_finished);
        second_observations.append(second_finished);
    }
    require(first_observations.output.contains("task-a") && second_observations.output.contains("task-b"),
            "writes to distinct exec sessions blocked each other: " + first_observations.diagnostics + "\n" +
                second_observations.diagnostics);
    require(first_observations.exited && first_observations.exit_zero && second_observations.exited && second_observations.exit_zero,
            "distinct exec sessions did not run through completion: " + first_observations.diagnostics + "\n" +
                second_observations.diagnostics);
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

void test_command_output_is_utf8() {
    ToolSet tools(std::filesystem::current_path());
#ifdef _WIN32
    auto call = make_call("utf8", "exec_command", R"({"cmd":"Write-Output 'héllo 你好'"})");
#else
    auto call = make_call("utf8", "exec_command", R"({"cmd":"printf 'héllo 你好\n'"})");
#endif
    auto outcome = execute(tools, std::move(call));
    require(outcome.has_value() && !tool_outcome_is_error(outcome->kind), "the UTF-8 echo command must succeed");
    require(outcome->payload.contains("héllo 你好"), "non-ASCII command output must reach the model as UTF-8, got: " + outcome->payload);

#ifdef _WIN32
    // Statements that must open a script have to survive the encoding setup.
    auto leading = execute(
        tools,
        make_call("using", "exec_command",
                  R"({"cmd":"using namespace System.Text; param($x = 'first'); [StringBuilder]::new(\"$x héllo\").ToString(); exit 3"})"));
    require(leading.has_value() && leading->receipt.contains("exit_code: 3") && leading->payload.contains("first héllo"),
            "using and param statements must remain the first statements of the user's script, got: " +
                (leading ? leading->receipt + "\n" + leading->payload : leading.error().message()));
    auto env = execute(
        tools, make_call("env", "exec_command", R"json({"cmd":"Write-Output ('path:' + [string]::IsNullOrEmpty($env:PATH))"})json"));
    require(env.has_value() && env->payload.contains("path:False"), "the exec shell must inherit the parent environment");
#endif
}

void test_nonzero_command_is_an_error_result() {
    ToolSet tools(std::filesystem::current_path());
#ifdef _WIN32
    auto call = make_call("nonzero", "exec_command", R"({"cmd":"Write-Error 'failed'; exit 7"})");
#else
    auto call = make_call("nonzero", "exec_command", R"({"cmd":"printf 'failed\\n' >&2; exit 7"})");
#endif
    auto outcome = execute(tools, std::move(call));
    require(outcome.has_value() && tool_outcome_is_error(outcome->kind) && outcome->receipt.contains("exit_code: 7"),
            "a nonzero shell exit must remain a tool result while entering the failed UI state");
}

i32 run_all() {
    test_tools_are_available_by_default();
    test_tool_registry_dispatches_extensions();
    test_tool_presentations_are_specific_and_bounded();
    test_read_file_is_bounded_and_regular();
    test_apply_patch_operations_are_validated_before_writes();
    test_apply_patch_resolves_files_when_executed();
    test_apply_patch_insert_only_hunk_lands_after_its_anchor();
    test_shell_task_interaction();
    test_shell_session_eof_and_kill();
    test_shell_output_receipt_is_resumable();
    test_distinct_shell_tasks_interact_concurrently();
    test_command_output_is_utf8();
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
