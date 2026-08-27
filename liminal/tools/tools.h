#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/common.h>

namespace liminal {

struct ShellTaskManager;

struct ShellTaskManagerDeleter {
    void operator()(ShellTaskManager *tasks) const;
};

using ShellTaskManagerPtr = std::unique_ptr<ShellTaskManager, ShellTaskManagerDeleter>;

enum struct ToolExecutionMode {
    EXCLUSIVE,
    PARALLEL,
};

inline constexpr usize k_max_tool_output_bytes = 64 * 1024;

/// Bound for one local tool result at the instant it begins executing.
/// Built-ins should use this while producing resumable output; the scheduler
/// applies the final shared provider-call allowance after all calls settle.
struct ToolExecutionContext {
    usize max_output_bytes = k_max_tool_output_bytes;
};

/// Replace invalid UTF-8 and enforce a byte bound. This is the universal guard
/// used after every local tool outcome has been normalized to a ToolResult.
void sanitize_tool_result(provider::ToolResult &result, usize maximum_bytes);

/// Bounded, user-facing data for the built-in tool lifecycle. Commands remain
/// separate so renderers can apply state copy and platform shell highlighting.
struct ToolCallPresentation {
    std::string description;
    std::string command;
};

struct ToolSet;

/// One model-callable tool and its local runtime behavior. Registrations own
/// their callbacks so future built-ins and extension sources share the same
/// dispatch path.
struct ToolRegistration {
    provider::ToolDefinition definition;
    /// Parallel tools may overlap with adjacent parallel calls. Exclusive
    /// tools form an ordering barrier and are the conservative default for
    /// extensions that may touch shared state.
    ToolExecutionMode execution_mode = ToolExecutionMode::EXCLUSIVE;
    std::move_only_function<Result<void>(const provider::ToolCall &) const> validate;
    std::move_only_function<lighter::Task<provider::ToolResult, Error>(const ToolSet &, const provider::ToolCall &,
                                                                       const ToolExecutionContext &) const>
        execute;
    std::move_only_function<ToolCallPresentation(const provider::ToolCall &) const> describe;
    std::move_only_function<std::string(const provider::ToolCall &, const provider::ToolResult &) const> summarize;
};

struct ToolSet {
    explicit ToolSet(std::filesystem::path working_directory);
    ~ToolSet();

    ToolSet(const ToolSet &) = delete;
    ToolSet &operator=(const ToolSet &) = delete;
    ToolSet(ToolSet &&) = delete;
    ToolSet &operator=(ToolSet &&) = delete;

    /// Adds a tool to this agent. Duplicate or incomplete registrations are
    /// rejected instead of silently shadowing an existing tool.
    Result<void> register_tool(ToolRegistration tool);

    std::vector<provider::ToolDefinition> definitions() const;

    /// Execute one tool call. Expected tool failures (bad path, nonzero exit)
    /// come back as a successful ToolResultBlock; only infrastructure
    /// failures (unknown tool, malformed input, spawn error) use the error
    /// channel - the agent layer converts those into is_error results.
    Result<void> validate(const provider::ToolCall &call) const;
    lighter::Task<provider::ToolResult, Error> execute(const provider::ToolCall &call, ToolExecutionContext context = {}) const;
    ToolExecutionMode execution_mode(std::string_view name) const;
    ToolCallPresentation describe(const provider::ToolCall &call) const;
    std::string summarize(const provider::ToolCall &call, const provider::ToolResult &result) const;

    std::filesystem::path working_directory;

private:
    ShellTaskManagerPtr shell_tasks;
    std::vector<ToolRegistration> registrations;
};

} // namespace liminal
