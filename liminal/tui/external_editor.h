#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>

#include <liminal/error.h>

namespace liminal::tui {

struct ExternalEditorCommand {
    std::vector<std::string> arguments;
};

/// Parse one platform-native editor command, including optional arguments.
Result<ExternalEditorCommand> parse_external_editor_command(std::string_view command);

/// Resolve VISUAL, falling back to EDITOR only when VISUAL is unset.
Result<ExternalEditorCommand> resolve_external_editor_command();

/// Edit a draft through a unique temporary Markdown file. The child inherits
/// the current working directory and all three standard streams.
lighter::Task<std::string, Error> run_external_editor(std::string_view seed, const ExternalEditorCommand &command);

} // namespace liminal::tui
