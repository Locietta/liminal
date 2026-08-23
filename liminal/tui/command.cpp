#include "command.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <lighter/encoding/utf8.h>

namespace liminal::tui {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

constexpr std::array<std::string_view, 1> k_quit_aliases{"exit"};

constexpr std::array<CommandSpec, 10> k_command_registry{{
    {.kind = CommandKind::HELP, .name = "help", .synopsis = "", .description = "list every command"},
    {.kind = CommandKind::MODEL, .name = "model", .synopsis = "[selector]", .description = "select the agent model"},
    {.kind = CommandKind::CONTEXT, .name = "context", .synopsis = "", .description = "show what occupies the model context"},
    {.kind = CommandKind::COMPACT, .name = "compact", .synopsis = "[instructions]", .description = "compact the conversation history"},
    {.kind = CommandKind::COPY, .name = "copy", .synopsis = "[reply number]", .description = "copy an assistant reply to the clipboard"},
    {.kind = CommandKind::NAME, .name = "name", .synopsis = "<title> | --clear", .description = "set or clear the session title"},
    {.kind = CommandKind::NEW, .name = "new", .synopsis = "", .description = "start a fresh session", .idle_only = true},
    {.kind = CommandKind::RESUME,
     .name = "resume",
     .synopsis = "",
     .description = "switch to another session in this workspace",
     .idle_only = true},
    {.kind = CommandKind::HISTORY, .name = "history", .synopsis = "", .description = "browse conversation checkpoints", .idle_only = true},
    {.kind = CommandKind::QUIT, .name = "quit", .aliases = k_quit_aliases, .synopsis = "", .description = "leave liminal"},
}};

} // namespace

std::span<const CommandSpec> command_registry() noexcept { return k_command_registry; }

const CommandSpec *find_command(std::string_view name) noexcept {
    for (const auto &spec : k_command_registry) {
        if (spec.name == name) return &spec;
        if (std::ranges::contains(spec.aliases, name)) return &spec;
    }
    return nullptr;
}

std::string describe_commands() {
    lighter::types::usize name_column = 0;
    for (const auto &spec : k_command_registry) {
        auto width = spec.name.size() + 1;
        if (!spec.synopsis.empty()) width += spec.synopsis.size() + 1;
        name_column = std::max(name_column, width);
    }

    std::string text = "commands:\n";
    for (const auto &spec : k_command_registry) {
        std::string entry = "  /" + std::string(spec.name);
        if (!spec.synopsis.empty()) {
            entry += ' ';
            entry += spec.synopsis;
        }
        entry.append(name_column + 4 - (entry.size() - 2), ' ');
        entry += spec.description;
        for (const auto &alias : spec.aliases) {
            entry += " (also /" + std::string(alias) + ")";
        }
        if (spec.idle_only) entry += " (idle only)";
        text += entry;
        text += '\n';
    }
    text += "type // to send a prompt that starts with /\n";
    return text;
}

Result<ReplInput> parse_repl_input(std::string text) {
    if (text.starts_with("//")) {
        text.erase(0, 1);
        return ReplInput{std::in_place_type<UserPrompt>, std::move(text)};
    }
    if (!text.starts_with('/')) {
        return ReplInput{std::in_place_type<UserPrompt>, std::move(text)};
    }

    std::string_view command(text);
    command.remove_prefix(1);
    const auto name_end = command.find_first_of(" \t\r\n\f\v");
    const auto name = command.substr(0, name_end);
    if (name.empty()) {
        return lighter::outcome_error(Error::command("empty slash command"));
    }
    const auto arguments = name_end == std::string_view::npos ? std::string_view{} : trim(command.substr(name_end));
    return ReplInput{std::in_place_type<CommandLine>, std::string(name), std::string(arguments)};
}

Result<CommandKind> resolve_command(std::string_view name) {
    if (const auto *spec = find_command(name)) return spec->kind;
    return lighter::outcome_error(Error::command("unknown command '/" + std::string(name) + "'"));
}

Result<CopyArguments> parse_copy_arguments(std::string_view arguments) {
    arguments = trim(arguments);
    if (arguments.empty()) return CopyArguments{};

    lighter::types::usize ordinal = 0;
    const auto parsed = std::from_chars(arguments.data(), arguments.data() + arguments.size(), ordinal);
    if (parsed.ec != std::errc{} || parsed.ptr != arguments.data() + arguments.size() || ordinal == 0) {
        return lighter::outcome_error(Error::command("usage: /copy [positive reply number]"));
    }
    return CopyArguments{.ordinal = ordinal};
}

Result<NameArguments> parse_name_arguments(std::string_view arguments) {
    arguments = trim(arguments);
    if (arguments == "--clear") return NameArguments{};
    if (arguments.empty() || arguments.size() > 200 || !lighter::encoding::utf8::is_valid(arguments)) {
        return lighter::outcome_error(Error::command("usage: /name <title> | /name --clear"));
    }
    return NameArguments{.title = std::string(arguments)};
}

Result<void> require_no_arguments(std::string_view command, std::string_view arguments) {
    if (trim(arguments).empty()) return {};
    return lighter::outcome_error(Error::command("usage: /" + std::string(command)));
}

} // namespace liminal::tui
