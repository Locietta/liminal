#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

struct WorkspaceIdentity {
    std::string root;
    std::string key;
};

struct StatePaths {
    std::filesystem::path root;

    std::filesystem::path catalog() const;
    std::filesystem::path sessions() const;
    std::filesystem::path staging() const;
    std::filesystem::path catalog_pending() const;
    std::filesystem::path locks() const;
    std::filesystem::path session_directory(SessionId id) const;
    std::filesystem::path session_database(SessionId id) const;
    std::filesystem::path pending_marker(SessionId id) const;
};

Result<std::filesystem::path> state_root_path();
Result<WorkspaceIdentity> workspace_identity(const std::filesystem::path &root);

} // namespace liminal::session
