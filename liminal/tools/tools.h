#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/common.h>

namespace liminal {

struct ToolPolicy {
    std::chrono::milliseconds command_timeout{120'000};
    lighter::usize max_parallel_calls = 4;
    lighter::usize max_calls_per_turn = 32;
};

/// Load resource limits for built-in tools.
Result<ToolPolicy> load_tool_policy();

/// Bounded, user-facing descriptions for the built-in tool lifecycle. These
/// never expose provider wire values directly to a terminal renderer.
std::string describe_tool_call(const provider::ToolCall &call);
std::string summarize_tool_result(const provider::ToolCall &call, const provider::ToolResult &result);

/// The v1 built-in tools: read_file and run_command (PowerShell on Windows,
/// POSIX sh on Linux).
/// Deliberately not a generic registry - two tools need two branches.
struct ToolSet {
    explicit ToolSet(std::filesystem::path working_directory, ToolPolicy policy = {});

    std::vector<provider::ToolDefinition> definitions() const;

    /// Execute one tool call. Expected tool failures (bad path, nonzero exit)
    /// come back as a successful ToolResultBlock; only infrastructure
    /// failures (unknown tool, malformed input, spawn error) use the error
    /// channel - the agent layer converts those into is_error results.
    lighter::Task<provider::ToolResult, Error> execute(const provider::ToolCall &call) const;

    std::filesystem::path working_directory;
    ToolPolicy policy;
};

} // namespace liminal
