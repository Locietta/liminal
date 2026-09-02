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
#include <liminal/tools/outcome.h>

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

inline constexpr usize k_max_tool_payload_bytes = 64 * 1024;
inline constexpr usize k_max_tool_receipt_bytes = 64 * 1024;

/// Immutable admission issued before a tool may execute. Receipt capacity is
/// reserved control plane; only payload capacity is distributable output.
struct ToolOutputGrant {
    usize receipt_bytes;
    usize payload_bytes;
};

/// Sanitize optional output within its grant without changing outcome
/// semantics. Exceeding reserved receipt capacity is a tool contract failure.
void finalize_tool_outcome(ToolOutcome &outcome, ToolOutputGrant grant);

/// Bounded, user-facing data for the built-in tool lifecycle. Commands remain
/// separate so renderers can apply state copy and platform shell highlighting.
struct ToolCallPresentation {
    std::string description;
    std::string command;
};

struct ToolSet;

struct PreparedToolCall {
    provider::ToolCall call;
    ToolExecutionMode mode = ToolExecutionMode::EXCLUSIVE;
    usize receipt_bytes = 0;
    usize minimum_payload_bytes = 0;
    std::move_only_function<lighter::Task<ToolOutcome, Error>(ToolOutputGrant)> execute;
};

struct PreparedToolExecution {
    usize receipt_bytes = 0;
    usize minimum_payload_bytes = 0;
    std::move_only_function<lighter::Task<ToolOutcome, Error>(ToolOutputGrant)> execute;
};

/// One model-callable tool and its local runtime behavior. Registrations own
/// their callbacks so future built-ins and extension sources share the same
/// dispatch path.
struct ToolRegistration {
    provider::ToolDefinition definition;
    /// Parallel tools may overlap with adjacent parallel calls. Exclusive
    /// tools form an ordering barrier and are the conservative default for
    /// extensions that may touch shared state.
    ToolExecutionMode execution_mode = ToolExecutionMode::EXCLUSIVE;
    usize receipt_bytes = 0;
    usize minimum_payload_bytes = 0;
    /// Optional read-only preparation for stateful tools. It must perform no
    /// externally visible side effects and returns the exact receipt bound
    /// needed before its invocation can be admitted.
    std::move_only_function<Result<PreparedToolExecution>(const ToolSet &, const provider::ToolCall &) const> prepare;
    std::move_only_function<Result<void>(const provider::ToolCall &) const> validate;
    std::move_only_function<lighter::Task<ToolOutcome, Error>(const ToolSet &, const provider::ToolCall &, ToolOutputGrant) const> execute;
    /// Presentation callbacks cannot fail: the display layer must always be
    /// told what a call is and how it ended, and there is no meaningful
    /// recovery from a presentation error. Report tool failures through the
    /// outcome, never by throwing here.
    std::move_only_function<ToolCallPresentation(const provider::ToolCall &) const noexcept> describe;
    std::move_only_function<std::string(const provider::ToolCall &, const ToolOutcome &) const noexcept> summarize;
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

    /// Read-only preparation. The returned invocation cannot execute without
    /// an explicit grant from the batch planner.
    Result<PreparedToolCall> prepare(provider::ToolCall call) const;
    lighter::Task<ToolOutcome, Error> execute(provider::ToolCall call, ToolOutputGrant grant) const;
    ToolExecutionMode execution_mode(std::string_view name) const;
    ToolCallPresentation describe(const provider::ToolCall &call) const noexcept;
    std::string summarize(const provider::ToolCall &call, const ToolOutcome &outcome) const noexcept;

    std::filesystem::path working_directory;

private:
    ShellTaskManagerPtr shell_tasks;
    std::vector<std::unique_ptr<ToolRegistration>> registrations;
};

} // namespace liminal
