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
                large->content.ends_with("[truncated after 131072 bytes]"),
            "read_file did not return a bounded prefix for a large file");

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

i32 run_all() {
    test_tools_are_available_by_default();
    test_read_file_is_bounded_and_regular();
    test_command_timeout();
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
