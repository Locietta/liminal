#pragma once

#include <optional>
#include <span>
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
    RESUME,
    NAME,
    HISTORY,
    HELP,
};

/// Presentation and resolution metadata for one slash command. The registry is
/// the single authority for names, aliases, and user-facing descriptions;
/// execution stays with the dispatcher.
struct CommandSpec {
    CommandKind kind;
    std::string_view name;
    std::span<const std::string_view> aliases;
    std::string_view synopsis;
    std::string_view description;
    bool idle_only = false;
};

/// Every executable slash command, exactly once, in presentation order.
std::span<const CommandSpec> command_registry() noexcept;

/// Finds a command by exact canonical name or alias; nullptr when unknown.
const CommandSpec *find_command(std::string_view name) noexcept;

/// Renders the durable registry-backed command reference used by /help.
std::string describe_commands();

/// Resolves exact command names and their centrally registered aliases.
Result<CommandKind> resolve_command(std::string_view name);

struct CopyArguments {
    lighter::types::usize ordinal = 1;
};

Result<CopyArguments> parse_copy_arguments(std::string_view arguments);

struct NameArguments {
    std::optional<std::string> title;
};

Result<NameArguments> parse_name_arguments(std::string_view arguments);
Result<void> require_no_arguments(std::string_view command, std::string_view arguments);

} // namespace liminal::tui
