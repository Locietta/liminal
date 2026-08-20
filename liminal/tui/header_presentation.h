#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <lighter/types.hpp>

namespace liminal::tui {

using namespace lighter::types;

/// Semantic session identity. Values are retained independently of terminal
/// width; frames request a disposable, cell-bounded projection.
struct SessionHeader {
    std::string workspace_path = ".";
    std::optional<std::string> home_directory;
    std::optional<std::string> explicit_title;
    std::string prompt_preview;
};

/// Semantic description of one header row. Contextual full-screen surfaces
/// reuse the same workspace and title projection as the normal session.
struct HeaderContent {
    std::string identity;
    SessionHeader session;
    bool include_session_title = false;
};

/// Collapses layout controls and whitespace into a terminal-safe single line.
std::string normalize_header_text(std::string_view text);

/// Resolves explicit title, first-prompt preview, then the new-session label.
std::string resolve_session_title(const SessionHeader &header);

/// Projects a foreign- or host-style path without relying on host filesystem
/// parsing. The result never exceeds `columns` terminal cells.
std::string present_workspace_path(std::string_view path, const std::optional<std::string> &home_directory, i32 columns);

/// Projects identity, workspace, and optional title into one terminal row.
std::string present_header(const HeaderContent &content, i32 columns);

} // namespace liminal::tui
