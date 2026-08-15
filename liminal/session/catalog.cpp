#include "catalog.h"

#include "catalog_lease.h"
#include "durable_fs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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

Result<bool> initialize(sqlite3 *database) {
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) return lighter::outcome_error(std::move(begun).error());
    const auto fail = [database](Error error) -> Result<bool> {
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
        return false;
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
    auto created = execute(database, R"sql(
CREATE TABLE sessions (
    id BLOB PRIMARY KEY CHECK(length(id) = 16),
    observed_revision INTEGER NOT NULL,
    workspace_key TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    title TEXT,
    preview TEXT NOT NULL
);
CREATE INDEX sessions_workspace_recent ON sessions(workspace_key, updated_at_ms DESC, id DESC);
PRAGMA application_id = 1279872323;
PRAGMA user_version = 1;
)sql");
    if (!created) return fail(std::move(created).error());
    if (auto committed = execute(database, "COMMIT"); !committed) return lighter::outcome_error(std::move(committed).error());
    return true;
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
    std::filesystem::path path;
    sqlite3 *database = nullptr;
    bool created = false;
    std::optional<detail::CatalogLease> maintenance_lease;
    std::optional<detail::CatalogLease> initialization_lease;
    mutable std::mutex mutex;
};

Result<SessionCatalog> SessionCatalog::open(const std::filesystem::path &state_root) {
    std::scoped_lock process_lock(catalog_open_mutex);
    std::error_code error;
    const auto created_root = std::filesystem::create_directories(state_root, error);
    if (error) return lighter::outcome_error(Error::storage("cannot create state root: " + error.message()));
    const auto created_locks = std::filesystem::create_directories(state_root / "locks", error);
    if (error) return lighter::outcome_error(Error::storage("cannot create catalog lock directory: " + error.message()));
#ifndef _WIN32
    for (const auto &[directory, created] :
         std::array{std::pair{state_root, created_root}, std::pair{state_root / "locks", created_locks}}) {
        if (!created) continue;
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
        if (error) return lighter::outcome_error(Error::storage("cannot secure state directory: " + error.message()));
    }
#endif
    for (const auto &directory : {state_root, state_root / "locks"}) {
        auto reparse = detail::is_reparse_point(directory);
        if (!reparse) return lighter::outcome_error(std::move(reparse).error());
        if (*reparse)
            return lighter::outcome_error(
                Error::storage("state path is a symlink, junction, or reparse point: " + directory.generic_string()));
    }
    auto initialization_lease = detail::acquire_catalog_initialization_lease(state_root);
    if (!initialization_lease) return lighter::outcome_error(std::move(initialization_lease).error());
    auto maintenance_lease = detail::acquire_catalog_lease(state_root, false);
    if (!maintenance_lease) return lighter::outcome_error(std::move(maintenance_lease).error());
    auto state = std::make_shared<State>();
    state->maintenance_lease = *std::move(maintenance_lease);
    state->path = state_root / "catalog.sqlite3";
    const auto catalog_exists = std::filesystem::exists(state->path, error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect session catalog: " + error.message()));
    if (catalog_exists) {
        auto reparse = detail::is_reparse_point(state->path);
        if (!reparse) return lighter::outcome_error(std::move(reparse).error());
        if (*reparse) return lighter::outcome_error(Error::storage("session catalog is a symlink, junction, or reparse point"));
    }
    const auto encoded = path_utf8(state->path);
    const auto code =
        sqlite3_open_v2(encoded.c_str(), &state->database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
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
    auto initialized = initialize(state->database);
    if (!initialized) return lighter::outcome_error(std::move(initialized).error());
    state->created = *initialized;
    if (state->created) state->initialization_lease = *std::move(initialization_lease);
    auto wal = prepare(state->database, "PRAGMA journal_mode=WAL");
    if (!wal) return lighter::outcome_error(std::move(wal).error());
    if (sqlite3_step(wal->value) != SQLITE_ROW || text(wal->value, 0) != "wal") {
        return lighter::outcome_error(Error::storage("session catalog did not enter WAL journal mode"));
    }
    return SessionCatalog(std::move(state));
}

Result<SessionCatalog> SessionCatalog::repair_corrupt(const std::filesystem::path &state_root) {
    const auto catalog = state_root / "catalog.sqlite3";
    std::error_code error;
    if (std::filesystem::exists(catalog, error)) {
        auto reparse = detail::is_reparse_point(catalog);
        if (!reparse) return lighter::outcome_error(std::move(reparse).error());
        if (*reparse) return lighter::outcome_error(Error::storage("refusing to replace a catalog symlink, junction, or reparse point"));
    }
    if (error) return lighter::outcome_error(Error::storage("cannot inspect corrupt catalog: " + error.message()));
    if (!std::filesystem::is_regular_file(catalog, error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect corrupt catalog: " + error.message()));
        return lighter::outcome_error(Error::storage("cannot repair an absent session catalog"));
    }
    {
        auto maintenance = detail::acquire_catalog_lease(state_root, true);
        if (!maintenance) return lighter::outcome_error(std::move(maintenance).error());
        const auto suffix = ".corrupt." + to_string(generate_session_id());
        std::filesystem::rename(catalog, catalog.string() + suffix, error);
        if (error) return lighter::outcome_error(Error::storage("cannot preserve corrupt catalog for replacement: " + error.message()));
        for (const auto sidecar : {std::string("-wal"), std::string("-shm")}) {
            const auto source = std::filesystem::path(catalog.string() + sidecar);
            if (!std::filesystem::exists(source, error)) {
                if (error) return lighter::outcome_error(Error::storage("cannot inspect corrupt catalog sidecar: " + error.message()));
                continue;
            }
            std::filesystem::rename(source, source.string() + suffix, error);
            if (error) return lighter::outcome_error(Error::storage("cannot preserve corrupt catalog sidecar: " + error.message()));
        }
    }
    auto replacement = open(state_root);
    if (!replacement) return lighter::outcome_error(std::move(replacement).error());
    replacement->state->created = true;
    return *std::move(replacement);
}

const std::filesystem::path &SessionCatalog::path() const noexcept { return state->path; }
bool SessionCatalog::was_created() const noexcept { return state->created; }
void SessionCatalog::finish_initialization() const { state->initialization_lease.reset(); }

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

Result<std::vector<SessionId>> SessionCatalog::ids() const {
    std::scoped_lock lock(state->mutex);
    auto query = prepare(state->database, "SELECT id FROM sessions ORDER BY id");
    if (!query) return lighter::outcome_error(std::move(query).error());
    std::vector<SessionId> result;
    while (true) {
        const auto row = sqlite3_step(query->value);
        if (row == SQLITE_DONE) break;
        if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot enumerate catalog identities", row));
        auto id = column_id(query->value, 0);
        if (!id) return lighter::outcome_error(std::move(id).error());
        result.push_back(*id);
    }
    return result;
}

Result<void> SessionCatalog::upsert(const CatalogProjection &projection) const {
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
    bind_id(query->value, 1, projection.summary.id);
    sqlite3_bind_int64(query->value, 2, static_cast<sqlite3_int64>(projection.observed_revision));
    sqlite3_bind_text(query->value, 3, projection.workspace_key.data(), static_cast<int>(projection.workspace_key.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(query->value, 4, projection.summary.updated_at_ms);
    if (projection.summary.title)
        sqlite3_bind_text(query->value, 5, projection.summary.title->data(), static_cast<int>(projection.summary.title->size()),
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(query->value, 5);
    sqlite3_bind_text(query->value, 6, projection.summary.preview.data(), static_cast<int>(projection.summary.preview.size()),
                      SQLITE_TRANSIENT);
    const auto row = sqlite3_step(query->value);
    if (row != SQLITE_DONE) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(sqlite_error(state->database, "cannot update session projection", row));
    }
    auto committed = execute(state->database, "COMMIT");
    if (!committed) {
        if (rollback) static_cast<void>(execute(state->database, "ROLLBACK"));
        return committed;
    }
    rollback = false;
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
