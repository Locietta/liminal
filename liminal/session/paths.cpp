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

Result<std::filesystem::path> state_database_path() {
    if (auto override = environment("LIMINAL_STATE_DB")) return std::filesystem::path(*override);
#ifdef _WIN32
    auto local = environment("LOCALAPPDATA");
    if (!local) return lighter::outcome_error(Error::storage("LOCALAPPDATA is not set"));
    return std::filesystem::path(*local) / "Liminal" / "state.sqlite3";
#else
    if (auto state = environment("XDG_STATE_HOME")) return std::filesystem::path(*state) / "liminal" / "state.sqlite3";
    auto home = environment("HOME");
    if (!home) return lighter::outcome_error(Error::storage("neither XDG_STATE_HOME nor HOME is set"));
    return std::filesystem::path(*home) / ".local" / "state" / "liminal" / "state.sqlite3";
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
