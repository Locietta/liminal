#include "catalog.h"

#include "catalog_lease.h"
#include "catalog_validation.h"
#include "durable_fs.h"
#include "paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>

#include <sqlite3.h>

#include <lighter/async/vocab/outcome.h>
#include <lighter/utils/panic.h>

namespace liminal::session {

namespace {

constexpr int k_catalog_application_id = 0x4c494d43;
constexpr int k_catalog_schema_version = 1;
constexpr auto k_healthy_catalog_initialization_wait = std::chrono::seconds(1);
std::mutex catalog_open_mutex;

std::string path_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

Error sqlite_error(sqlite3 *database, std::string_view action, int code = SQLITE_ERROR) {
    auto detail = std::string(action) + ": " + (database ? sqlite3_errmsg(database) : sqlite3_errstr(code));
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session catalog busy: " + detail;
    return Error::storage(std::move(detail));
}

struct Statement {
    sqlite3_stmt *value = nullptr;
    Statement() = default;
    Statement(Statement &&other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Statement &operator=(Statement &&other) noexcept {
        if (this == &other) return *this;
        if (value) sqlite3_finalize(value);
        value = std::exchange(other.value, nullptr);
        return *this;
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    ~Statement() {
        if (value) sqlite3_finalize(value);
    }
};

Result<Statement> prepare(sqlite3 *database, std::string_view sql) {
    Statement statement;
    const auto code = sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement.value, nullptr);
    if (code != SQLITE_OK) return lighter::outcome_error(sqlite_error(database, "cannot prepare catalog query", code));
    return statement;
}

Result<void> execute(sqlite3 *database, std::string_view sql) {
    char *message = nullptr;
    const auto code = sqlite3_exec(database, sql.data(), nullptr, nullptr, &message);
    if (code == SQLITE_OK) return {};
    auto detail = std::string("cannot update session catalog: ") + (message ? message : sqlite3_errmsg(database));
    sqlite3_free(message);
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session catalog busy: " + detail;
    return lighter::outcome_error(Error::storage(std::move(detail)));
}

void bind_id(sqlite3_stmt *statement, int index, SessionId id) {
    sqlite3_bind_blob(statement, index, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
}

Result<SessionId> column_id(sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB || sqlite3_column_bytes(statement, column) != 16) {
        return lighter::outcome_error(Error::storage("session catalog contains an invalid UUID"));
    }
    SessionId id;
    std::memcpy(id.bytes.data(), sqlite3_column_blob(statement, column), id.bytes.size());
    return id;
}

std::optional<std::string> optional_text(sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
    return std::string(value, value + sqlite3_column_bytes(statement, column));
}

std::string text(sqlite3_stmt *statement, int column) { return optional_text(statement, column).value_or(""); }

Result<void> bind_and_step_projection(sqlite3 *database, sqlite3_stmt *statement, const CatalogProjection &projection) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    bind_id(statement, 1, projection.summary.id);
    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(projection.observed_revision));
    sqlite3_bind_text(statement, 3, projection.workspace_key.data(), static_cast<int>(projection.workspace_key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, projection.summary.updated_at_ms);
    if (projection.summary.title)
        sqlite3_bind_text(statement, 5, projection.summary.title->data(), static_cast<int>(projection.summary.title->size()),
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(statement, 5);
    sqlite3_bind_text(statement, 6, projection.summary.preview.data(), static_cast<int>(projection.summary.preview.size()),
                      SQLITE_TRANSIENT);
    const auto row = sqlite3_step(statement);
    if (row != SQLITE_DONE) return lighter::outcome_error(sqlite_error(database, "cannot update session projection", row));
    return {};
}

enum struct CatalogInitialization {
    READY,
    NEEDS_CREATION,
};

Result<void> prepare_state_directory(const std::filesystem::path &path, std::string_view description) {
    auto type = detail::inspect_path_no_follow(path);
    if (!type) return lighter::outcome_error(std::move(type).error());
    bool created = false;
    if (*type == detail::PathType::ABSENT) {
        std::error_code error;
        created = std::filesystem::create_directories(path, error);
        if (error) return lighter::outcome_error(Error::storage("cannot create " + std::string(description) + ": " + error.message()));
        type = detail::inspect_path_no_follow(path);
        if (!type) return lighter::outcome_error(std::move(type).error());
    }
    if (*type == detail::PathType::REPARSE_POINT) {
        return lighter::outcome_error(
            Error::storage(std::string(description) + " is a symlink, junction, or reparse point: " + path.generic_string()));
    }
    if (*type != detail::PathType::DIRECTORY) {
        return lighter::outcome_error(Error::storage(std::string(description) + " is not a directory: " + path.generic_string()));
    }
#ifndef _WIN32
    if (created) {
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
        if (error) return lighter::outcome_error(Error::storage("cannot secure state directory: " + error.message()));
    }
#endif
    return {};
}

Result<void> prepare_catalog_directories(const std::filesystem::path &state_root) {
    if (auto prepared = prepare_state_directory(state_root, "state root"); !prepared) {
        return lighter::outcome_error(std::move(prepared).error());
    }
    if (auto prepared = prepare_state_directory(state_root / "locks", "catalog lock directory"); !prepared) {
        return lighter::outcome_error(std::move(prepared).error());
    }
    return {};
}

Result<void> validate_rebuild_marker(const std::filesystem::path &marker) {
    auto type = detail::inspect_path_no_follow(marker);
    if (!type) return lighter::outcome_error(std::move(type).error());
    if (*type == detail::PathType::REPARSE_POINT) {
        return lighter::outcome_error(
            Error::storage("catalog rebuild marker is a symlink, junction, or reparse point", ErrorCode::INVALID_DATA));
    }
    if (*type != detail::PathType::REGULAR_FILE) {
        return lighter::outcome_error(Error::storage("catalog rebuild marker is not a regular file", ErrorCode::INVALID_DATA));
    }
    std::ifstream input(marker, std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(input), {});
    if (!input || contents != "1\n") {
        return lighter::outcome_error(Error::storage("catalog rebuild marker has invalid format", ErrorCode::INVALID_DATA));
    }
    return {};
}

Result<std::optional<SessionId>> read_repair_marker(const std::filesystem::path &marker) {
    auto type = detail::inspect_path_no_follow(marker);
    if (!type) return lighter::outcome_error(std::move(type).error());
    if (*type == detail::PathType::ABSENT) return std::optional<SessionId>{};
    if (*type == detail::PathType::REPARSE_POINT) {
        return lighter::outcome_error(
            Error::storage("catalog repair marker is a symlink, junction, or reparse point", ErrorCode::INVALID_DATA));
    }
    if (*type != detail::PathType::REGULAR_FILE) {
        return lighter::outcome_error(Error::storage("catalog repair marker is not a regular file", ErrorCode::INVALID_DATA));
    }
    std::ifstream input(marker, std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(input), {});
    if (!input || contents.size() != 37 || contents.back() != '\n') {
        return lighter::outcome_error(Error::storage("catalog repair marker has invalid format", ErrorCode::INVALID_DATA));
    }
    const auto text = std::string_view(contents).substr(0, contents.size() - 1);
    auto id = parse_session_id(text);
    if (!id || to_string(*id) != text) {
        return lighter::outcome_error(Error::storage("catalog repair marker has invalid format", ErrorCode::INVALID_DATA));
    }
    return std::optional<SessionId>{*id};
}

Result<SessionId> ensure_repair_marker(const std::filesystem::path &marker) {
    auto pending = read_repair_marker(marker);
    if (!pending) return lighter::outcome_error(std::move(pending).error());
    if (*pending) return **pending;
    const auto id = generate_session_id();
    if (auto marked = detail::durable_replace_file(marker, to_string(id) + "\n"); !marked) {
        return lighter::outcome_error(Error::storage("cannot mark catalog repair incomplete: " + marked.error().message()));
    }
    return id;
}

Result<void> quarantine_catalog_family(const std::filesystem::path &catalog, SessionId quarantine_id) {
    const auto quarantine_suffix = std::filesystem::path{".corrupt." + to_string(quarantine_id)};
    const auto family = std::array{detail::path_with_suffix(catalog, std::filesystem::path{"-journal"}),
                                   detail::path_with_suffix(catalog, std::filesystem::path{"-wal"}),
                                   detail::path_with_suffix(catalog, std::filesystem::path{"-shm"}), catalog};
    for (const auto &source : family) {
        const auto target = detail::path_with_suffix(source, quarantine_suffix);
        auto type = detail::inspect_path_no_follow(source);
        if (!type) return lighter::outcome_error(std::move(type).error());
        auto target_type = detail::inspect_path_no_follow(target);
        if (!target_type) return lighter::outcome_error(std::move(target_type).error());
        if (*target_type != detail::PathType::ABSENT && *target_type != detail::PathType::REGULAR_FILE) {
            return lighter::outcome_error(
                Error::storage("catalog quarantine path has an invalid filesystem type", ErrorCode::INVALID_DATA));
        }
        if (*type == detail::PathType::ABSENT) continue;
        if (*type != detail::PathType::REGULAR_FILE) {
            return lighter::outcome_error(
                Error::storage("refusing to quarantine an invalid catalog database-family path", ErrorCode::INVALID_DATA));
        }
        if (*target_type != detail::PathType::ABSENT) {
            return lighter::outcome_error(
                Error::storage("live and quarantined catalog database-family paths both exist", ErrorCode::INVALID_DATA));
        }
        std::error_code error;
        std::filesystem::rename(source, target, error);
        if (error) {
            return lighter::outcome_error(Error::storage("cannot quarantine catalog database family: " + error.message()));
        }
    }
    return {};
}

Result<void> prepare_catalog_rebuild(const std::filesystem::path &catalog, const std::filesystem::path &repair_marker,
                                     const std::filesystem::path &rebuild_marker, bool require_quarantine) {
    auto repair = read_repair_marker(repair_marker);
    if (!repair) return lighter::outcome_error(std::move(repair).error());
    auto repair_id = *std::move(repair);
    if (require_quarantine && !repair_id) {
        auto marked = ensure_repair_marker(repair_marker);
        if (!marked) return lighter::outcome_error(std::move(marked).error());
        repair_id = *marked;
    }
    if (!repair_id) return {};
    if (auto quarantined = quarantine_catalog_family(catalog, *repair_id); !quarantined) {
        return lighter::outcome_error(std::move(quarantined).error());
    }
    if (auto marked = detail::durable_replace_file(rebuild_marker, "1\n"); !marked) {
        return lighter::outcome_error(Error::storage("cannot mark catalog rebuild incomplete: " + marked.error().message()));
    }
    if (auto completed = detail::durable_remove_file(repair_marker); !completed) {
        return lighter::outcome_error(Error::storage("cannot complete catalog-family quarantine: " + completed.error().message()));
    }
    return {};
}

Result<CatalogInitialization> initialize(sqlite3 *database, bool allow_creation) {
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) return lighter::outcome_error(std::move(begun).error());
    const auto fail = [database](Error error) -> Result<CatalogInitialization> {
        static_cast<void>(execute(database, "ROLLBACK"));
        return lighter::outcome_error(std::move(error));
    };
    auto application = prepare(database, "PRAGMA application_id");
    if (!application) return fail(std::move(application).error());
    if (sqlite3_step(application->value) != SQLITE_ROW) return fail(sqlite_error(database, "cannot read catalog identity"));
    const auto application_id = sqlite3_column_int(application->value, 0);
    sqlite3_finalize(application->value);
    application->value = nullptr;
    auto version = prepare(database, "PRAGMA user_version");
    if (!version) return fail(std::move(version).error());
    if (sqlite3_step(version->value) != SQLITE_ROW) return fail(sqlite_error(database, "cannot read catalog version"));
    const auto schema_version = sqlite3_column_int(version->value, 0);
    if (application_id == k_catalog_application_id && schema_version == k_catalog_schema_version) {
        if (auto committed = execute(database, "COMMIT"); !committed) return lighter::outcome_error(std::move(committed).error());
        return CatalogInitialization::READY;
    }
    if (application_id != 0 || schema_version != 0) {
        return fail(Error::storage("session catalog has a foreign or unsupported schema"));
    }
    auto objects = prepare(database, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
    if (!objects) return fail(std::move(objects).error());
    if (sqlite3_step(objects->value) != SQLITE_ROW) return fail(sqlite_error(database, "cannot inspect catalog schema"));
    if (sqlite3_column_int64(objects->value, 0) != 0) {
        return fail(Error::storage("unidentified non-empty SQLite database cannot be used as the session catalog"));
    }
    if (!allow_creation) {
        if (auto rolled_back = execute(database, "ROLLBACK"); !rolled_back) return lighter::outcome_error(std::move(rolled_back).error());
        return CatalogInitialization::NEEDS_CREATION;
    }
    auto created = execute(database, R"sql(
CREATE TABLE sessions (
    id BLOB PRIMARY KEY CHECK(length(id) = 16),
    observed_revision INTEGER NOT NULL CHECK(observed_revision>0),
    workspace_key TEXT NOT NULL CHECK(length(workspace_key)>0),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms>0),
    title TEXT,
    preview TEXT NOT NULL
);
CREATE INDEX sessions_workspace_recent ON sessions(workspace_key, updated_at_ms DESC, id DESC);
PRAGMA application_id = 1279872323;
PRAGMA user_version = 1;
)sql");
    if (!created) return fail(std::move(created).error());
    if (auto committed = execute(database, "COMMIT"); !committed) return lighter::outcome_error(std::move(committed).error());
    return CatalogInitialization::READY;
}

u8 hexadecimal_nibble(char character) noexcept {
    if (character >= '0' && character <= '9') return static_cast<u8>(character - '0');
    if (character >= 'a' && character <= 'f') return static_cast<u8>(character - 'a' + 10);
    return static_cast<u8>(character - 'A' + 10);
}

void set_nibble(SessionId &id, usize index, u8 value) noexcept {
    auto &byte = id.bytes[index / 2];
    if (index % 2 == 0)
        byte = static_cast<u8>((byte & 0x0f) | (value << 4));
    else
        byte = static_cast<u8>((byte & 0xf0) | value);
}

std::pair<SessionId, std::optional<SessionId>> prefix_bounds(std::string_view prefix) {
    SessionId lower;
    for (usize index = 0; index < prefix.size(); ++index) set_nibble(lower, index, hexadecimal_nibble(prefix[index]));
    auto upper = lower;
    for (usize index = prefix.size(); index-- > 0;) {
        const auto value = hexadecimal_nibble(prefix[index]);
        if (value == 0xf) continue;
        set_nibble(upper, index, static_cast<u8>(value + 1));
        for (usize trailing = index + 1; trailing < 32; ++trailing) set_nibble(upper, trailing, 0);
        return {lower, upper};
    }
    return {lower, std::nullopt};
}

} // namespace

struct SessionCatalog::State {
    ~State() {
        if (database) sqlite3_close(database);
    }
    std::filesystem::path root;
    std::filesystem::path path;
    sqlite3 *database = nullptr;
    std::optional<detail::CatalogLease> initialization_lease;
    std::optional<detail::CatalogLease> maintenance_lease;
    bool rebuild_exclusive = false;
    mutable std::mutex mutex;
};

Result<SessionCatalog> SessionCatalog::open(const std::filesystem::path &state_root) {
    std::scoped_lock process_lock(catalog_open_mutex);
    if (auto prepared = prepare_catalog_directories(state_root); !prepared) return lighter::outcome_error(std::move(prepared).error());
    const auto catalog_path = state_root / "catalog.sqlite3";
    auto catalog_type = detail::inspect_path_no_follow(catalog_path);
    if (!catalog_type) return lighter::outcome_error(std::move(catalog_type).error());
    if (*catalog_type == detail::PathType::REPARSE_POINT)
        return lighter::outcome_error(Error::storage("session catalog is a symlink, junction, or reparse point", ErrorCode::INVALID_DATA));
    if (*catalog_type != detail::PathType::ABSENT && *catalog_type != detail::PathType::REGULAR_FILE)
        return lighter::outcome_error(Error::storage("session catalog is not a regular file", ErrorCode::INVALID_DATA));
    const auto repair_marker = StatePaths{state_root}.catalog_repair_marker();
    auto repair = read_repair_marker(repair_marker);
    if (!repair) return lighter::outcome_error(std::move(repair).error());
    const auto rebuild_marker = StatePaths{state_root}.catalog_rebuild_marker();
    auto marker_type = detail::inspect_path_no_follow(rebuild_marker);
    if (!marker_type) return lighter::outcome_error(std::move(marker_type).error());
    if (*marker_type == detail::PathType::REPARSE_POINT)
        return lighter::outcome_error(
            Error::storage("catalog rebuild marker is a symlink, junction, or reparse point", ErrorCode::INVALID_DATA));
    if (*marker_type != detail::PathType::ABSENT && *marker_type != detail::PathType::REGULAR_FILE)
        return lighter::outcome_error(Error::storage("catalog rebuild marker is not a regular file", ErrorCode::INVALID_DATA));
    if (*marker_type == detail::PathType::REGULAR_FILE) {
        if (auto valid = validate_rebuild_marker(rebuild_marker); !valid) {
            return lighter::outcome_error(std::move(valid).error());
        }
    }
    auto catalog_exists = *catalog_type == detail::PathType::REGULAR_FILE;
    auto rebuild_pending = *marker_type == detail::PathType::REGULAR_FILE;
    auto repair_pending = repair->has_value();
    const auto wait =
        catalog_exists && !repair_pending && !rebuild_pending ? k_healthy_catalog_initialization_wait : std::chrono::milliseconds{};
    auto initialization_lease = detail::acquire_catalog_initialization_lease(state_root, wait);
    if (!initialization_lease) return lighter::outcome_error(std::move(initialization_lease).error());
    catalog_type = detail::inspect_path_no_follow(catalog_path);
    if (!catalog_type) return lighter::outcome_error(std::move(catalog_type).error());
    marker_type = detail::inspect_path_no_follow(rebuild_marker);
    if (!marker_type) return lighter::outcome_error(std::move(marker_type).error());
    repair = read_repair_marker(repair_marker);
    if (!repair) return lighter::outcome_error(std::move(repair).error());
    if (*catalog_type == detail::PathType::REPARSE_POINT || *marker_type == detail::PathType::REPARSE_POINT)
        return lighter::outcome_error(Error::storage("catalog initialization paths changed to a reparse point", ErrorCode::INVALID_DATA));
    if ((*catalog_type != detail::PathType::ABSENT && *catalog_type != detail::PathType::REGULAR_FILE) ||
        (*marker_type != detail::PathType::ABSENT && *marker_type != detail::PathType::REGULAR_FILE)) {
        return lighter::outcome_error(
            Error::storage("catalog initialization paths have an invalid filesystem type", ErrorCode::INVALID_DATA));
    }
    if (*marker_type == detail::PathType::REGULAR_FILE) {
        if (auto valid = validate_rebuild_marker(rebuild_marker); !valid) {
            return lighter::outcome_error(std::move(valid).error());
        }
    }
    catalog_exists = *catalog_type == detail::PathType::REGULAR_FILE;
    rebuild_pending = *marker_type == detail::PathType::REGULAR_FILE;
    repair_pending = repair->has_value();
    auto state = std::make_shared<State>();
    state->root = state_root;
    state->path = catalog_path;
    auto needs_rebuild = !catalog_exists || repair_pending || rebuild_pending;
    auto maintenance_lease = detail::acquire_catalog_lease(state_root, needs_rebuild);
    if (!maintenance_lease) return lighter::outcome_error(std::move(maintenance_lease).error());
    state->maintenance_lease = *std::move(maintenance_lease);
    state->rebuild_exclusive = needs_rebuild;
    if (needs_rebuild) state->initialization_lease = *std::move(initialization_lease);
    const auto prepare_rebuild = [&](bool require_quarantine) -> Result<void> {
        if (auto prepared = prepare_catalog_rebuild(catalog_path, repair_marker, rebuild_marker, require_quarantine); !prepared) {
            return lighter::outcome_error(std::move(prepared).error());
        }
        needs_rebuild = true;
        return {};
    };
    if (needs_rebuild) {
        if (auto prepared = prepare_rebuild(repair_pending || (!catalog_exists && !rebuild_pending)); !prepared) {
            return lighter::outcome_error(std::move(prepared).error());
        }
    }
    const auto open_database = [&]() -> Result<void> {
        if (auto valid = detail::validate_sqlite_paths_no_follow(state->path, needs_rebuild); !valid) {
            return lighter::outcome_error(std::move(valid).error());
        }
        const auto encoded = path_utf8(state->path);
        const auto code =
            sqlite3_open_v2(encoded.c_str(), &state->database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
        if (code != SQLITE_OK) return lighter::outcome_error(sqlite_error(state->database, "cannot open session catalog", code));
        if (sqlite3_libversion_number() < 3051003 || sqlite3_libversion_number() >= 4000000) {
            return lighter::outcome_error(Error::storage("SQLite 3.51.3 or newer (and older than 4.0) is required"));
        }
        if (sqlite3_busy_timeout(state->database, 1000) != SQLITE_OK) {
            return lighter::outcome_error(sqlite_error(state->database, "cannot configure catalog busy timeout"));
        }
        if (auto configured = execute(state->database, "PRAGMA synchronous=FULL;"); !configured) {
            return lighter::outcome_error(std::move(configured).error());
        }
        return {};
    };
    if (auto opened = open_database(); !opened) return lighter::outcome_error(std::move(opened).error());
    auto initialized = initialize(state->database, needs_rebuild);
    if (!initialized) return lighter::outcome_error(std::move(initialized).error());
    if (*initialized == CatalogInitialization::NEEDS_CREATION) {
        if (sqlite3_close(state->database) != SQLITE_OK)
            return lighter::outcome_error(Error::storage("cannot close unidentified session catalog"));
        state->database = nullptr;
        state->maintenance_lease.reset();
        auto exclusive = detail::acquire_catalog_lease(state_root, true);
        if (!exclusive) return lighter::outcome_error(std::move(exclusive).error());
        state->maintenance_lease = *std::move(exclusive);
        state->rebuild_exclusive = true;
        state->initialization_lease = *std::move(initialization_lease);
        if (auto prepared = prepare_rebuild(true); !prepared) return lighter::outcome_error(std::move(prepared).error());
        if (auto opened = open_database(); !opened) return lighter::outcome_error(std::move(opened).error());
        initialized = initialize(state->database, true);
        if (!initialized) return lighter::outcome_error(std::move(initialized).error());
    }
    auto wal = prepare(state->database, "PRAGMA journal_mode=WAL");
    if (!wal) return lighter::outcome_error(std::move(wal).error());
    if (sqlite3_step(wal->value) != SQLITE_ROW || text(wal->value, 0) != "wal") {
        return lighter::outcome_error(Error::storage("session catalog did not enter WAL journal mode"));
    }
    if (auto valid = detail::validate_sqlite_paths_no_follow(state->path, false); !valid) {
        return lighter::outcome_error(std::move(valid).error());
    }
    return SessionCatalog(std::move(state));
}

Result<SessionCatalog> SessionCatalog::repair_corrupt(const std::filesystem::path &state_root) {
    if (auto prepared = prepare_catalog_directories(state_root); !prepared) return lighter::outcome_error(std::move(prepared).error());
    const auto replaced = [&]() -> Result<void> {
        auto initialization = detail::acquire_catalog_initialization_lease(state_root);
        if (!initialization) return lighter::outcome_error(std::move(initialization).error());
        const auto catalog = state_root / "catalog.sqlite3";
        const auto repair_marker = StatePaths{state_root}.catalog_repair_marker();
        const auto rebuild_marker = StatePaths{state_root}.catalog_rebuild_marker();
        auto repair = read_repair_marker(repair_marker);
        if (!repair) return lighter::outcome_error(std::move(repair).error());
        auto marker_type = detail::inspect_path_no_follow(rebuild_marker);
        if (!marker_type) return lighter::outcome_error(std::move(marker_type).error());
        const auto rebuild_pending = *marker_type == detail::PathType::REGULAR_FILE;
        if (*marker_type != detail::PathType::ABSENT) {
            if (auto valid = validate_rebuild_marker(rebuild_marker); !valid) {
                return lighter::outcome_error(std::move(valid).error());
            }
        }
        if (auto valid = detail::validate_sqlite_paths_no_follow(catalog, rebuild_pending || repair->has_value()); !valid) {
            return lighter::outcome_error(std::move(valid).error());
        }
        auto maintenance = detail::acquire_catalog_lease(state_root, true);
        if (!maintenance) return lighter::outcome_error(std::move(maintenance).error());
        if (auto prepared = prepare_catalog_rebuild(catalog, repair_marker, rebuild_marker, !rebuild_pending); !prepared) {
            return lighter::outcome_error(std::move(prepared).error());
        }
        return {};
    }();
    if (!replaced) return lighter::outcome_error(std::move(replaced).error());
    auto replacement = open(state_root);
    if (!replacement) return lighter::outcome_error(std::move(replacement).error());
    return *std::move(replacement);
}

const std::filesystem::path &SessionCatalog::path() const noexcept { return state->path; }

bool SessionCatalog::owns_rebuild_exclusivity() const noexcept {
    return state->rebuild_exclusive && state->initialization_lease.has_value() && state->maintenance_lease.has_value();
}

Result<void> SessionCatalog::complete_rebuild() const {
    if (!owns_rebuild_exclusivity()) {
        return lighter::outcome_error(Error::storage("catalog rebuild completion does not own exclusive maintenance"));
    }
    state->maintenance_lease.reset();
    auto shared = detail::acquire_catalog_lease(state->root, false);
    if (!shared) return lighter::outcome_error(std::move(shared).error());
    state->maintenance_lease = *std::move(shared);
    state->rebuild_exclusive = false;
    state->initialization_lease.reset();
    return {};
}

Result<SessionId> SessionCatalog::resolve_prefix(std::string_view value) const {
    std::string prefix;
    prefix.reserve(value.size());
    for (const auto character : value) {
        if (character == '-') continue;
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return lighter::outcome_error(Error::config("invalid session id prefix"));
        }
        prefix.push_back(character);
    }
    if (prefix.size() < 8 || prefix.size() > 32) {
        return lighter::outcome_error(Error::config("session id prefix must contain between 8 and 32 hexadecimal digits"));
    }
    const auto [lower, upper] = prefix_bounds(prefix);
    std::scoped_lock lock(state->mutex);
    auto matches = upper ? prepare(state->database, "SELECT id FROM sessions WHERE id>=?1 AND id<?2 ORDER BY id LIMIT 2") :
                           prepare(state->database, "SELECT id FROM sessions WHERE id>=?1 ORDER BY id LIMIT 2");
    if (!matches) return lighter::outcome_error(std::move(matches).error());
    bind_id(matches->value, 1, lower);
    if (upper) bind_id(matches->value, 2, *upper);
    std::optional<SessionId> result;
    while (true) {
        const auto row = sqlite3_step(matches->value);
        if (row == SQLITE_DONE) break;
        if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot resolve session prefix", row));
        auto id = column_id(matches->value, 0);
        if (!id) return lighter::outcome_error(std::move(id).error());
        if (result) return lighter::outcome_error(Error::storage("session id prefix is ambiguous"));
        result = *id;
    }
    if (!result) return lighter::outcome_error(Error::storage("session was not found"));
    return *result;
}

Result<SessionSummary> SessionCatalog::latest(std::string_view workspace_key) const {
    auto result = page({.workspace_key = std::string(workspace_key), .limit = 1});
    if (!result) return lighter::outcome_error(std::move(result).error());
    if (result->sessions.empty()) return lighter::outcome_error(Error::storage("no saved session exists in this workspace"));
    return std::move(result->sessions.front());
}

Result<SessionPage> SessionCatalog::page(const SessionPageQuery &request) const {
    lighter::check(request.limit > 0, "session catalog page size must be positive");
    constexpr usize k_maximum_page_size = 50;
    const auto page_size = std::min(request.limit, k_maximum_page_size);
    const auto continued = request.after.has_value();
    const auto sql = continued ? R"sql(
SELECT id,updated_at_ms,title,preview FROM sessions
WHERE workspace_key=?1 AND (updated_at_ms,id)<(?2,?3)
ORDER BY updated_at_ms DESC,id DESC LIMIT ?4
)sql" :
                                 R"sql(
SELECT id,updated_at_ms,title,preview FROM sessions
WHERE workspace_key=?1 ORDER BY updated_at_ms DESC,id DESC LIMIT ?2
)sql";
    std::scoped_lock lock(state->mutex);
    auto query = prepare(state->database, sql);
    if (!query) return lighter::outcome_error(std::move(query).error());
    sqlite3_bind_text(query->value, 1, request.workspace_key.data(), static_cast<int>(request.workspace_key.size()), SQLITE_TRANSIENT);
    const auto limit = static_cast<sqlite3_int64>(page_size + 1);
    if (request.after) {
        sqlite3_bind_int64(query->value, 2, request.after->updated_at_ms);
        bind_id(query->value, 3, request.after->id);
        sqlite3_bind_int64(query->value, 4, limit);
    } else {
        sqlite3_bind_int64(query->value, 2, limit);
    }
    SessionPage page;
    while (page.sessions.size() <= page_size) {
        const auto row = sqlite3_step(query->value);
        if (row == SQLITE_DONE) break;
        if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot list sessions", row));
        auto id = column_id(query->value, 0);
        if (!id) return lighter::outcome_error(std::move(id).error());
        page.sessions.push_back({.id = *id,
                                 .updated_at_ms = sqlite3_column_int64(query->value, 1),
                                 .title = optional_text(query->value, 2),
                                 .preview = text(query->value, 3)});
    }
    if (page.sessions.size() > page_size) {
        page.sessions.pop_back();
        const auto &last = page.sessions.back();
        page.continuation = SessionPageCursor{.updated_at_ms = last.updated_at_ms, .id = last.id};
    }
    return page;
}

Result<std::optional<CatalogProjection>> SessionCatalog::find(SessionId id) const {
    std::scoped_lock lock(state->mutex);
    auto query = prepare(state->database, "SELECT observed_revision,workspace_key,updated_at_ms,title,preview FROM sessions WHERE id=?1");
    if (!query) return lighter::outcome_error(std::move(query).error());
    bind_id(query->value, 1, id);
    const auto row = sqlite3_step(query->value);
    if (row == SQLITE_DONE) return std::optional<CatalogProjection>{};
    if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot read session projection", row));
    return std::optional<CatalogProjection>{CatalogProjection{
        .summary = {.id = id,
                    .updated_at_ms = sqlite3_column_int64(query->value, 2),
                    .title = optional_text(query->value, 3),
                    .preview = text(query->value, 4)},
        .observed_revision = static_cast<u64>(sqlite3_column_int64(query->value, 0)),
        .workspace_key = text(query->value, 1),
    }};
}

Result<void> SessionCatalog::upsert(const CatalogProjection &projection) const {
    if (auto valid = detail::validate_catalog_projection(projection); !valid) return lighter::outcome_error(std::move(valid).error());
    std::scoped_lock lock(state->mutex);
    auto transaction = execute(state->database, "BEGIN IMMEDIATE");
    if (!transaction) return transaction;
    bool rollback = true;
    auto query = prepare(state->database, R"sql(
INSERT INTO sessions(id,observed_revision,workspace_key,updated_at_ms,title,preview) VALUES(?1,?2,?3,?4,?5,?6)
ON CONFLICT(id) DO UPDATE SET observed_revision=excluded.observed_revision,workspace_key=excluded.workspace_key,
 updated_at_ms=excluded.updated_at_ms,title=excluded.title,preview=excluded.preview
WHERE excluded.observed_revision>=sessions.observed_revision
)sql");
    if (!query) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(std::move(query).error());
    }
    if (auto updated = bind_and_step_projection(state->database, query->value, projection); !updated) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return updated;
    }
    auto committed = execute(state->database, "COMMIT");
    if (!committed) {
        if (rollback) static_cast<void>(execute(state->database, "ROLLBACK"));
        return committed;
    }
    rollback = false;
    return {};
}

Result<void> SessionCatalog::replace_all(std::span<const CatalogProjection> projections) const {
    for (const auto &projection : projections) {
        if (auto valid = detail::validate_catalog_projection(projection); !valid) return lighter::outcome_error(std::move(valid).error());
    }
    std::scoped_lock lock(state->mutex);
    if (auto transaction = execute(state->database, "BEGIN IMMEDIATE"); !transaction) return transaction;
    const auto rollback = [this](Error error) -> Result<void> {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(std::move(error));
    };
    if (auto cleared = execute(state->database, "DELETE FROM sessions"); !cleared) return rollback(std::move(cleared).error());
    auto insert = prepare(state->database, R"sql(
INSERT INTO sessions(id,observed_revision,workspace_key,updated_at_ms,title,preview) VALUES(?1,?2,?3,?4,?5,?6)
)sql");
    if (!insert) return rollback(std::move(insert).error());
    for (const auto &projection : projections) {
        if (auto inserted = bind_and_step_projection(state->database, insert->value, projection); !inserted) {
            return rollback(std::move(inserted).error());
        }
    }
    if (auto committed = execute(state->database, "COMMIT"); !committed) return rollback(std::move(committed).error());
    return {};
}

Result<void> SessionCatalog::remove(SessionId id) const {
    std::scoped_lock lock(state->mutex);
    auto query = prepare(state->database, "DELETE FROM sessions WHERE id=?1");
    if (!query) return lighter::outcome_error(std::move(query).error());
    bind_id(query->value, 1, id);
    const auto row = sqlite3_step(query->value);
    if (row != SQLITE_DONE) return lighter::outcome_error(sqlite_error(state->database, "cannot remove stale catalog row", row));
    return {};
}

} // namespace liminal::session
