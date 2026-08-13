#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <liminal/error.h>

namespace liminal::session {

struct WorkspaceIdentity {
    std::string root;
    std::string key;
};

Result<std::filesystem::path> state_database_path();
Result<WorkspaceIdentity> workspace_identity(const std::filesystem::path &root);

} // namespace liminal::session
