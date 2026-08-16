#include "durable_fs.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

namespace {

Result<void> validate_regular_path(const std::filesystem::path &path, std::string_view description, bool allow_absent) {
    auto type = inspect_path_no_follow(path);
    if (!type) return lighter::outcome_error(std::move(type).error());
    if (*type == PathType::ABSENT && allow_absent) return {};
    if (*type == PathType::REPARSE_POINT) {
        return lighter::outcome_error(
            Error::storage(std::string(description) + " is a symlink, junction, or reparse point", ErrorCode::INVALID_DATA));
    }
    if (*type == PathType::ABSENT) {
        return lighter::outcome_error(Error::storage(std::string(description) + " is absent", ErrorCode::NOT_FOUND));
    }
    if (*type != PathType::REGULAR_FILE) {
        return lighter::outcome_error(Error::storage(std::string(description) + " is not a regular file", ErrorCode::INVALID_DATA));
    }
    return {};
}

} // namespace

Result<void> validate_sqlite_paths_no_follow(const std::filesystem::path &database, bool allow_database_creation) {
    if (auto valid = validate_regular_path(database, "SQLite database", allow_database_creation); !valid) {
        return lighter::outcome_error(std::move(valid).error());
    }
    for (const auto suffix : std::array{std::string_view{"-journal"}, std::string_view{"-wal"}, std::string_view{"-shm"}}) {
        if (auto valid = validate_regular_path(database.string() + std::string(suffix), "SQLite auxiliary file", true); !valid) {
            return lighter::outcome_error(std::move(valid).error());
        }
    }
    return {};
}

} // namespace liminal::session::detail
