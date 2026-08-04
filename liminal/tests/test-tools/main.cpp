#include <chrono>
#include <cstdio>
#include <filesystem>
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

void test_workspace_policy() {
    ToolSet tools(std::filesystem::current_path());
    auto definitions = tools.definitions();
    require(definitions.size() == 1 && definitions[0].name == "read_file", "workspace mode must not advertise a shell");

    auto readme = execute(tools, make_call("read", "read_file", R"({"path":"README.md"})"));
    require(readme.has_value() && !readme->is_error && readme->content.contains("Liminal"),
            "workspace mode must read files inside its root");

    auto escaped = execute(tools, make_call("escape", "read_file", R"({"path":"../../outside-liminal-workspace"})"));
    require(escaped.has_value() && escaped->is_error && escaped->content.contains("workspace policy rejects path"),
            "workspace mode must reject traversal outside its canonical root");

    auto command = execute(tools, make_call("command", "run_command", R"({"command":"pwd"})"));
    require(command.has_value() && command->is_error && command->content.contains("LIMINAL_TOOL_MODE=unrestricted"),
            "workspace mode must reject unadvertised command execution");
}

void test_unrestricted_command() {
    ToolPolicy policy{.mode = ToolMode::UNRESTRICTED};
    ToolSet tools(std::filesystem::current_path(), policy);
    auto definitions = tools.definitions();
    require(definitions.size() == 2 && definitions[1].name == "run_command", "unrestricted mode must advertise the shell");

    auto outcome = execute(tools, make_call("test-call", "run_command", R"({"command":"pwd"})"));
    require(outcome.has_value(), "run_command failed to execute");
    require(outcome->call_id == "test-call" && !outcome->is_error, "run_command returned an error result");
    require(outcome->content.contains("exit_code: 0"), "native command shell did not report success");
    require(outcome->content.contains("stdout:\n"), "native command shell did not capture stdout");
}

void test_command_timeout() {
    ToolPolicy policy{
        .mode = ToolMode::UNRESTRICTED,
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
    test_workspace_policy();
    test_unrestricted_command();
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
