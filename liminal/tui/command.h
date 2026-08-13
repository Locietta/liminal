#pragma once

#include <string>
#include <string_view>
#include <variant>

#include <lighter/types.hpp>

#include <liminal/error.h>

namespace liminal::tui {

struct UserPrompt {
    std::string text;
};

struct CommandLine {
    std::string name;
    std::string arguments;
};

using ReplInput = std::variant<UserPrompt, CommandLine>;

/// Separates ordinary prompts from slash-command syntax. A leading double
/// slash escapes one slash into an ordinary user prompt.
Result<ReplInput> parse_repl_input(std::string text);

enum struct CommandKind {
    QUIT,
    COPY,
    CONTEXT,
    COMPACT,
    MODEL,
};

/// Resolves exact command names and their centrally registered aliases.
Result<CommandKind> resolve_command(std::string_view name);

struct CopyArguments {
    lighter::types::usize ordinal = 1;
};

Result<CopyArguments> parse_copy_arguments(std::string_view arguments);
Result<void> require_no_arguments(std::string_view command, std::string_view arguments);

} // namespace liminal::tui
