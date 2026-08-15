#include "paths.h"

#include <cstdlib>
#include <system_error>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session {

#ifdef _WIN32
namespace detail {
Result<std::string> windows_workspace_key(const std::filesystem::path &path);
} // namespace detail
#endif

namespace {

std::optional<std::string> environment(const char *name) {
    const auto *value = std::getenv(name);
    if (!value || !*value) return std::nullopt;
    return std::string(value);
}

std::string path_text(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

} // namespace

std::filesystem::path StatePaths::catalog() const { return root / "catalog.sqlite3"; }
std::filesystem::path StatePaths::catalog_rebuild_marker() const { return root / "catalog-rebuild-pending"; }
std::filesystem::path StatePaths::sessions() const { return root / "sessions"; }
std::filesystem::path StatePaths::staging() const { return root / "staging"; }
std::filesystem::path StatePaths::catalog_pending() const { return root / "catalog-pending"; }
std::filesystem::path StatePaths::locks() const { return root / "locks"; }
std::filesystem::path StatePaths::session_directory(SessionId id) const { return sessions() / to_string(id); }
std::filesystem::path StatePaths::session_database(SessionId id) const { return session_directory(id) / "session.sqlite3"; }
std::filesystem::path StatePaths::pending_marker(SessionId id) const { return catalog_pending() / to_string(id); }

Result<std::filesystem::path> state_root_path() {
    if (auto override = environment("LIMINAL_STATE_DIR")) return std::filesystem::path(*override);
#ifdef _WIN32
    auto local = environment("LOCALAPPDATA");
    if (!local) return lighter::outcome_error(Error::storage("LOCALAPPDATA is not set"));
    return std::filesystem::path(*local) / "Liminal";
#else
    if (auto state = environment("XDG_STATE_HOME")) return std::filesystem::path(*state) / "liminal";
    auto home = environment("HOME");
    if (!home) return lighter::outcome_error(Error::storage("neither XDG_STATE_HOME nor HOME is set"));
    return std::filesystem::path(*home) / ".local" / "state" / "liminal";
#endif
}

Result<WorkspaceIdentity> workspace_identity(const std::filesystem::path &root) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return lighter::outcome_error(Error::storage("cannot resolve workspace '" + root.generic_string() + "': " + error.message()));
    }
    auto display = path_text(canonical);
#ifdef _WIN32
    auto key = detail::windows_workspace_key(canonical);
    if (!key) return lighter::outcome_error(std::move(key).error());
    return WorkspaceIdentity{.root = std::move(display), .key = *std::move(key)};
#else
    return WorkspaceIdentity{.root = display, .key = std::move(display)};
#endif
}

} // namespace liminal::session
