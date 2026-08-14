#include "command.h"

#include <charconv>
#include <cctype>
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

} // namespace

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
    if (name == "quit" || name == "exit") return CommandKind::QUIT;
    if (name == "copy") return CommandKind::COPY;
    if (name == "context") return CommandKind::CONTEXT;
    if (name == "compact") return CommandKind::COMPACT;
    if (name == "model") return CommandKind::MODEL;
    if (name == "resume") return CommandKind::RESUME;
    if (name == "name") return CommandKind::NAME;
    if (name == "archive") return CommandKind::ARCHIVE;
    if (name == "unarchive") return CommandKind::UNARCHIVE;
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
