#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <glaze/json.hpp>

#include <lighter/async/io/loop.h>
#include <lighter/types.hpp>

#include <liminal/provider/common.h>
#include <liminal/tools/tools.h>

namespace {

using namespace lighter::types;
using namespace liminal;
using namespace std::chrono_literals;

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
    require(definitions.size() == 2 && definitions[0].name == "read_file" && definitions[1].name == "run_command",
            "default tools must include file reading and shell execution");

    auto readme = execute(tools, make_call("read", "read_file", R"({"path":"../README.md"})"));
    require(readme.has_value() && !readme->is_error && readme->content.contains("Liminal"),
            "read_file must allow paths outside the working directory");

    auto command = execute(tools, make_call("command", "run_command", R"({"command":"pwd"})"));
    require(command.has_value() && command->call_id == "command" && !command->is_error && command->content.contains("exit_code: 0") &&
                command->content.contains("stdout:\n"),
            "run_command must execute without an opt-in mode");
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

    const auto command = make_call("command", "run_command", R"({"command":"pixi run build\npixi run test-unit"})");
    const auto presentation = tools.describe(command);
    require(presentation.description.empty() && presentation.command == "pixi run build\npixi run test-unit",
            "run_command presentation must retain the exact multiline command as semantic data");
    const provider::ToolResult command_result{
        .call_id = "command",
        .content = "exit_code: 7\n\nstdout:\nconfigured\nbuilt\n\nstderr:\none test failed\n",
        .is_error = true,
    };
    const auto summary = tools.summarize(command, command_result);
    require(summary.contains("exit 7") && summary.contains("stdout 2 lines") && summary.contains("stderr 1 line") &&
                summary.contains("configured") && summary.contains("one test failed"),
            "run_command completion must expose exit status, stream counts, and a bounded output preview");
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

void test_command_timeout() {
    ToolPolicy policy{
        .command_timeout = 25ms,
    };
    ToolSet tools(std::filesystem::current_path(), policy);
#ifdef _WIN32
    constexpr std::string_view command = R"({"command":"Start-Sleep -Seconds 5"})";
#else
    constexpr std::string_view command = R"({"command":"sleep 5"})";
#endif

    const auto started = std::chrono::steady_clock::now();
    auto outcome = execute(tools, make_call("timeout", "run_command", command));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(outcome.has_error() && outcome.error().kind == ErrorKind::TOOL && outcome.error().detail.contains("exceeded 25 ms"),
            "a command deadline must surface as a tool error");
    require(elapsed < 2s, "a timed-out command must be killed promptly");
}

void test_nonzero_command_is_an_error_result() {
    ToolSet tools(std::filesystem::current_path());
#ifdef _WIN32
    auto call = make_call("nonzero", "run_command", R"({"command":"Write-Error 'failed'; exit 7"})");
#else
    auto call = make_call("nonzero", "run_command", R"({"command":"printf 'failed\\n' >&2; exit 7"})");
#endif
    auto outcome = execute(tools, std::move(call));
    require(outcome.has_value() && outcome->is_error && outcome->content.starts_with("exit_code: 7"),
            "a nonzero shell exit must remain a tool result while entering the failed UI state");
}

i32 run_all() {
    test_tools_are_available_by_default();
    test_tool_registry_dispatches_extensions();
    test_tool_presentations_are_specific_and_bounded();
    test_read_file_is_bounded_and_regular();
    test_command_timeout();
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
