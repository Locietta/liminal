#include "external_editor.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/async/io/fs.h>
#include <lighter/async/io/process.h>
#include <lighter/async/runtime/sync.h>

namespace liminal::tui {

using lighter::fail;
using lighter::Process;
using lighter::Task;

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

bool command_space(char character) { return std::isspace(static_cast<unsigned char>(character)) != 0; }

#ifndef _WIN32
Result<ExternalEditorCommand> parse_native_command(std::string_view command) {
    ExternalEditorCommand result;
    usize index = 0;
    while (true) {
        while (index < command.size() && command_space(command[index])) ++index;
        if (index == command.size()) break;

        std::string argument;
        bool quoted = false;
        while (index < command.size() && (quoted || !command_space(command[index]))) {
            const auto character = command[index++];
            if (character == '\'') {
                quoted = true;
                const auto end = command.find('\'', index);
                if (end == std::string_view::npos) return lighter::outcome_error(Error::config("editor command has an unmatched quote"));
                argument.append(command.substr(index, end - index));
                index = end + 1;
                quoted = false;
                continue;
            }
            if (character == '"') {
                quoted = true;
                while (index < command.size() && command[index] != '"') {
                    if (command[index] == '\\' && index + 1 < command.size() &&
                        (command[index + 1] == '"' || command[index + 1] == '\\' || command[index + 1] == '$' ||
                         command[index + 1] == '`')) {
                        ++index;
                    }
                    argument += command[index++];
                }
                if (index == command.size()) return lighter::outcome_error(Error::config("editor command has an unmatched quote"));
                ++index;
                quoted = false;
                continue;
            }
            if (character == '\\') {
                if (index == command.size()) return lighter::outcome_error(Error::config("editor command ends with an escape"));
                argument += command[index++];
                continue;
            }
            argument += character;
        }
        result.arguments.push_back(std::move(argument));
    }
    return result;
}
#else
Result<ExternalEditorCommand> parse_native_command(std::string_view command) {
    ExternalEditorCommand result;
    usize index = 0;
    while (true) {
        while (index < command.size() && command_space(command[index])) ++index;
        if (index == command.size()) break;

        std::string argument;
        bool quoted = false;
        while (index < command.size() && (quoted || !command_space(command[index]))) {
            usize backslashes = 0;
            while (index < command.size() && command[index] == '\\') {
                ++backslashes;
                ++index;
            }
            if (index < command.size() && command[index] == '"') {
                argument.append(backslashes / 2, '\\');
                if (backslashes % 2 != 0) {
                    argument += '"';
                    ++index;
                } else if (quoted && index + 1 < command.size() && command[index + 1] == '"') {
                    argument += '"';
                    index += 2;
                } else {
                    quoted = !quoted;
                    ++index;
                }
                continue;
            }
            argument.append(backslashes, '\\');
            if (index < command.size() && (quoted || !command_space(command[index]))) argument += command[index++];
        }
        if (quoted) return lighter::outcome_error(Error::config("editor command has an unmatched quote"));
        result.arguments.push_back(std::move(argument));
    }
    return result;
}

std::vector<std::string> windows_extensions() {
    const char *raw = std::getenv("PATHEXT");
    std::string_view value = raw && *raw ? std::string_view(raw) : std::string_view(".COM;.EXE;.BAT;.CMD");
    std::vector<std::string> result;
    usize start = 0;
    while (start <= value.size()) {
        const auto end = value.find(';', start);
        if (end != start) result.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::filesystem::path resolve_windows_program(std::string_view program) {
    const auto requested = std::filesystem::path(program);
    std::vector<std::filesystem::path> roots;
    if (requested.has_parent_path()) {
        roots.push_back({});
    } else {
        roots.push_back(std::filesystem::current_path());
        if (const char *raw_path = std::getenv("PATH")) {
            const std::string_view path(raw_path);
            usize start = 0;
            while (start <= path.size()) {
                const auto end = path.find(';', start);
                if (end != start) roots.emplace_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }
    }

    auto extensions = requested.has_extension() ? std::vector<std::string>{""} : windows_extensions();
    for (const auto &root : roots) {
        for (const auto &extension : extensions) {
            auto candidate = requested;
            candidate += extension;
            if (!root.empty()) candidate = root / candidate;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        }
    }
    return requested;
}

std::string lowercase_extension(const std::filesystem::path &path) {
    auto result = path.extension().string();
    std::ranges::transform(result, result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

std::string quote_batch_argument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"&|<>^()") == std::string_view::npos) return std::string(argument);
    std::string result = "\"";
    for (const auto character : argument) {
        if (character == '"') result += '"';
        result += character;
    }
    return result + "\"";
}
#endif

Result<Process::Options> editor_process_options(const ExternalEditorCommand &command, const std::filesystem::path &draft) {
    if (command.arguments.empty() || command.arguments.front().empty()) {
        return lighter::outcome_error(Error::config("editor command is empty"));
    }

#ifndef _WIN32
    auto arguments = command.arguments;
    arguments.push_back(draft.string());
    return Process::Options{.file = arguments.front(), .args = std::move(arguments)};
#else
    auto arguments = command.arguments;
    const auto program = resolve_windows_program(arguments.front());
    arguments.front() = program.string();
    arguments.push_back(draft.string());
    const auto extension = lowercase_extension(program);
    if (extension != ".cmd" && extension != ".bat") {
        return Process::Options{.file = arguments.front(), .args = std::move(arguments)};
    }

    const char *configured_shell = std::getenv("COMSPEC");
    const auto shell = configured_shell && *configured_shell ? std::string(configured_shell) : std::string("cmd.exe");
    std::string batch_command;
    for (const auto &argument : arguments) {
        if (!batch_command.empty()) batch_command += ' ';
        batch_command += quote_batch_argument(argument);
    }
    batch_command = "\"" + batch_command + "\"";
    return Process::Options{
        .file = shell,
        .args = {quote_batch_argument(shell), "/d", "/s", "/c", std::move(batch_command)},
        .creation = {.windows_verbatim_arguments = true},
    };
#endif
}

} // namespace

Result<ExternalEditorCommand> parse_external_editor_command(std::string_view command) {
    auto parsed = parse_native_command(command);
    if (!parsed) return parsed;
    if (parsed->arguments.empty() || parsed->arguments.front().empty()) {
        return lighter::outcome_error(Error::config("editor command is empty"));
    }
    return parsed;
}

Result<ExternalEditorCommand> resolve_external_editor_command() {
    const char *raw = std::getenv("VISUAL");
    if (!raw) raw = std::getenv("EDITOR");
    if (!raw) return lighter::outcome_error(Error::config("set VISUAL or EDITOR before starting Liminal"));
    return parse_external_editor_command(raw);
}

Task<std::string, Error> run_external_editor(std::string_view seed, const ExternalEditorCommand &command) {
    std::error_code filesystem_error;
    const auto temporary_root = std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error) co_await fail(Error::tool("cannot find the temporary directory: " + filesystem_error.message()));

    auto created = co_await lighter::fs::mkdtemp((temporary_root / "liminal-editor-XXXXXX").string());
    if (!created) co_await fail(Error::tool("cannot create an editor temporary directory: " + std::string(created.error().message())));
    TemporaryDirectory temporary{.path = *std::move(created)};
    const auto draft = temporary.path / "prompt.md";
    {
        std::ofstream output(draft, std::ios::binary | std::ios::trunc);
        output.write(seed.data(), static_cast<std::streamsize>(seed.size()));
        if (!output) co_await fail(Error::tool("cannot write the temporary editor draft"));
    }

    auto options = editor_process_options(command, draft);
    if (!options) co_await fail(std::move(options).error());
    auto spawned = Process::spawn(*options);
    if (!spawned) co_await fail(Error::tool("cannot launch external editor: " + std::string(spawned.error().message())));
    auto child = *std::move(spawned);
    auto status = co_await child.proc.wait();
    if (!status) co_await fail(Error::tool("cannot wait for external editor: " + std::string(status.error().message())));
    if (status->status != 0 || status->term_signal != 0) {
        auto detail = "external editor exited with status " + std::to_string(status->status);
        if (status->term_signal != 0) detail += " (signal " + std::to_string(status->term_signal) + ")";
        co_await fail(Error::tool(std::move(detail)));
    }

    auto edited = lighter::fs::sync::read_to_string(draft.string());
    if (!edited) co_await fail(Error::tool("cannot read the edited draft: " + std::string(edited.error().message())));
    co_return *std::move(edited);
}

} // namespace liminal::tui
