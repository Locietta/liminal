#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/common.h>

namespace liminal {

struct ExecSessionManager;

struct ExecSessionManagerDeleter {
    void operator()(ExecSessionManager *sessions) const;
};

using ExecSessionManagerPtr = std::unique_ptr<ExecSessionManager, ExecSessionManagerDeleter>;

struct ToolPolicy {
    lighter::usize max_parallel_calls = 4;
    lighter::usize max_calls_per_turn = 32;
};

/// Load resource limits for built-in tools.
Result<ToolPolicy> load_tool_policy();

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
    std::move_only_function<lighter::Task<provider::ToolResult, Error>(const ToolSet &, const provider::ToolCall &) const> execute;
    std::move_only_function<ToolCallPresentation(const provider::ToolCall &) const> describe;
    std::move_only_function<std::string(const provider::ToolCall &, const provider::ToolResult &) const> summarize;
};

struct ToolSet {
    explicit ToolSet(std::filesystem::path working_directory, ToolPolicy policy = {});
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
    lighter::Task<provider::ToolResult, Error> execute(const provider::ToolCall &call) const;
    ToolCallPresentation describe(const provider::ToolCall &call) const;
    std::string summarize(const provider::ToolCall &call, const provider::ToolResult &result) const;

    std::filesystem::path working_directory;
    ToolPolicy policy;

private:
    ExecSessionManagerPtr exec_sessions;
    std::vector<ToolRegistration> registrations;
};

} // namespace liminal
