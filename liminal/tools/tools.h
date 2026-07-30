#pragma once

#include <string>
#include <vector>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/provider/common.h"

namespace liminal {

/// The v1 built-in tools: read_file and run_command (PowerShell).
/// Deliberately not a generic registry - two tools need two branches.
struct ToolSet {
    explicit ToolSet(std::string working_directory);

    std::vector<provider::ToolDefinition> definitions() const;

    /// Execute one tool call. Expected tool failures (bad path, nonzero exit)
    /// come back as a successful ToolResultBlock; only infrastructure
    /// failures (unknown tool, malformed input, spawn error) use the error
    /// channel - the agent layer converts those into is_error results.
    lighter::Task<provider::ToolResult, Error> execute(const provider::ToolCall &call) const;

    std::string working_directory;
};

} // namespace liminal
