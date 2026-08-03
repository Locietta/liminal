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

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

i32 run_all() {
    glz::generic input;
    auto parse_error = glz::read_json(input, R"({"command":"pwd"})");
    require(!parse_error, "failed to create run_command input");

    ToolSet tools(std::filesystem::current_path().string());
    provider::ToolCall call{
        .id = "test-call",
        .name = "run_command",
        .input = std::move(input),
    };

    lighter::EventLoop loop;
    auto task = tools.execute(call);
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.has_value(), "run_command failed to execute");
    require(outcome->call_id == call.id && !outcome->is_error, "run_command returned an error result");
    require(outcome->content.contains("exit_code: 0"), "native command shell did not report success");
    require(outcome->content.contains("stdout:\n"), "native command shell did not capture stdout");
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
