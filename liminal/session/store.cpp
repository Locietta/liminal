#include "store.h"
#include "store_test.h"

#include "catalog.h"
#include "catalog_lease.h"
#include "codec.h"
#include "durable_fs.h"
#include "lease.h"
#include "paths.h"
#include "repository.h"
#include "repository_state.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <sqlite3.h>

#include <lighter/async/vocab/outcome.h>
#include <lighter/encoding/utf8.h>

namespace liminal::session {

namespace {

constexpr int k_session_application_id = 0x4c494d53;
constexpr int k_session_schema_version = 1;
constexpr usize k_marker_maximum_size = 64;

void notify_storage_test_hook(const StorageHookSlot &hook, testing::StorageEvent event) { hook.notify(event); }

std::string path_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

Error sqlite_error(sqlite3 *database, std::string_view action, int code = SQLITE_ERROR) {
    auto detail = std::string(action) + ": " + (database ? sqlite3_errmsg(database) : sqlite3_errstr(code));
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session database busy: " + detail;
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
    if (code != SQLITE_OK) return lighter::outcome_error(sqlite_error(database, "cannot prepare session query", code));
    return statement;
}

Result<void> execute(sqlite3 *database, std::string_view sql) {
    char *message = nullptr;
    const auto code = sqlite3_exec(database, sql.data(), nullptr, nullptr, &message);
    if (code == SQLITE_OK) return {};
    auto detail = std::string("cannot update session database: ") + (message ? message : sqlite3_errmsg(database));
    sqlite3_free(message);
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session database busy: " + detail;
    return lighter::outcome_error(Error::storage(std::move(detail)));
}

void bind_id(sqlite3_stmt *statement, int index, SessionId id) {
    sqlite3_bind_blob(statement, index, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
}

void bind_optional_text(sqlite3_stmt *statement, int index, const std::optional<std::string> &value) {
    if (value)
        sqlite3_bind_text(statement, index, value->data(), static_cast<int>(value->size()), SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(statement, index);
}

void bind_optional_entry(sqlite3_stmt *statement, int index, const std::optional<EntryId> &value) {
    if (value)
        sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value->value));
    else
        sqlite3_bind_null(statement, index);
}

void bind_optional_session(sqlite3_stmt *statement, int index, const std::optional<SessionId> &value) {
    if (value)
        bind_id(statement, index, *value);
    else
        sqlite3_bind_null(statement, index);
}

std::optional<std::string> optional_text(sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
    return std::string(value, value + sqlite3_column_bytes(statement, column));
}

std::string text(sqlite3_stmt *statement, int column) { return optional_text(statement, column).value_or(""); }

Result<SessionId> column_id(sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB || sqlite3_column_bytes(statement, column) != 16) {
        return lighter::outcome_error(Error::storage("session database contains an invalid identity"));
    }
    SessionId id;
    std::memcpy(id.bytes.data(), sqlite3_column_blob(statement, column), id.bytes.size());
    return id;
}

Result<void> validate_session_schema(sqlite3 *database, bool create) {
    auto application = prepare(database, "PRAGMA application_id");
    if (!application) return lighter::outcome_error(std::move(application).error());
    if (sqlite3_step(application->value) != SQLITE_ROW)
        return lighter::outcome_error(sqlite_error(database, "cannot read session identity"));
    const auto application_id = sqlite3_column_int(application->value, 0);
    sqlite3_finalize(application->value);
    application->value = nullptr;
    auto version = prepare(database, "PRAGMA user_version");
    if (!version) return lighter::outcome_error(std::move(version).error());
    if (sqlite3_step(version->value) != SQLITE_ROW)
        return lighter::outcome_error(sqlite_error(database, "cannot read session schema version"));
    const auto schema_version = sqlite3_column_int(version->value, 0);
    if (application_id == k_session_application_id && schema_version == k_session_schema_version) return {};
    if (!create) {
        if (application_id != k_session_application_id)
            return lighter::outcome_error(Error::storage("session database belongs to another application"));
        return lighter::outcome_error(Error::storage("session database has an unsupported schema version"));
    }
    if (application_id != 0 || schema_version != 0) return lighter::outcome_error(Error::storage("staging database has a foreign schema"));
    auto objects = prepare(database, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
    if (!objects) return lighter::outcome_error(std::move(objects).error());
    if (sqlite3_step(objects->value) != SQLITE_ROW) return lighter::outcome_error(sqlite_error(database, "cannot inspect staging schema"));
    if (sqlite3_column_int64(objects->value, 0) != 0)
        return lighter::outcome_error(Error::storage("unidentified non-empty staging database"));
    return execute(database, R"sql(
BEGIN IMMEDIATE;
CREATE TABLE session (
    singleton INTEGER PRIMARY KEY CHECK(singleton=1),
    session_id BLOB NOT NULL UNIQUE CHECK(length(session_id)=16),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    workspace_root TEXT NOT NULL,
    workspace_key TEXT NOT NULL,
    working_directory TEXT NOT NULL,
    title TEXT,
    preview TEXT NOT NULL,
    active_leaf_entry_id INTEGER,
    next_entry_id INTEGER NOT NULL,
    next_task_id INTEGER NOT NULL,
    next_provider_call_id INTEGER NOT NULL,
    entry_count INTEGER NOT NULL,
    tokens_used INTEGER NOT NULL,
    last_provider TEXT,
    last_model TEXT,
    last_reasoning_effort TEXT,
    revision INTEGER NOT NULL,
    forked_from_session_id BLOB,
    forked_from_entry_id INTEGER
);
CREATE TABLE session_entries (
    entry_id INTEGER PRIMARY KEY,
    task_id INTEGER,
    provider_call_id INTEGER,
    parent_entry_id INTEGER REFERENCES session_entries(entry_id),
    kind INTEGER NOT NULL,
    payload_version INTEGER NOT NULL,
    payload_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX session_entries_children ON session_entries(parent_entry_id);
CREATE INDEX session_entries_provider_calls ON session_entries(task_id,provider_call_id,entry_id);
PRAGMA application_id = 1279872339;
PRAGMA user_version = 1;
COMMIT;
)sql");
}

Result<sqlite3 *> open_database(const std::filesystem::path &path, bool create, bool wal) {
    sqlite3 *database = nullptr;
    const auto encoded = path_utf8(path);
    const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | (create ? SQLITE_OPEN_CREATE : 0);
    const auto code = sqlite3_open_v2(encoded.c_str(), &database, flags, nullptr);
    if (code != SQLITE_OK) {
        const auto error = sqlite_error(database, create ? "cannot create staged session database" : "cannot open session database", code);
        if (database) sqlite3_close(database);
        return lighter::outcome_error(error);
    }
    if (sqlite3_libversion_number() < 3051003 || sqlite3_libversion_number() >= 4000000) {
        sqlite3_close(database);
        return lighter::outcome_error(Error::storage("SQLite 3.51.3 or newer (and older than 4.0) is required"));
    }
    if (auto configured = execute(database, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA busy_timeout=5000;"); !configured) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(configured).error());
    }
    if (auto valid = validate_session_schema(database, create); !valid) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(valid).error());
    }
    auto mode = prepare(database, wal ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE");
    if (!mode) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(mode).error());
    }
    const auto expected = wal ? "wal" : "delete";
    if (sqlite3_step(mode->value) != SQLITE_ROW || text(mode->value, 0) != expected) {
        sqlite3_close(database);
        return lighter::outcome_error(Error::storage(std::string("session database did not enter ") + expected + " journal mode"));
    }
    return database;
}

struct EncodedEntry {
    EntryId id;
    std::optional<EntryId> parent_id;
    i64 created_at_ms = 0;
    EncodedPayload payload;
};

struct PreparedDelta {
    SessionDelta delta;
    std::vector<EncodedEntry> entries;
};

struct DurableHead {
    bool exists = false;
    u64 revision = 0;
    u64 entry_count = 0;
    u64 next_task_id = 1;
    u64 next_provider_call_id = 1;
    u64 tokens_used = 0;
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
    std::optional<SessionWorkspace> workspace;
    std::optional<ForkOrigin> forked_from;
    std::optional<std::string> title;
    std::string preview;
};

Result<void> validate_delta(const SessionDelta &delta, const DurableHead &head) {
    if (!head.exists && delta.entries.empty())
        return lighter::outcome_error(Error::storage("cannot materialize a session without a semantic entry"));
    if (!delta.metadata.workspace || delta.metadata.workspace->root.empty() || delta.metadata.workspace->key.empty()) {
        return lighter::outcome_error(Error::storage("session delta has incomplete workspace metadata"));
    }
    if (delta.metadata.created_at_ms <= 0 || delta.metadata.updated_at_ms < delta.metadata.created_at_ms ||
        (head.exists && (delta.metadata.created_at_ms != head.created_at_ms || delta.metadata.updated_at_ms < head.updated_at_ms))) {
        return lighter::outcome_error(Error::storage("session delta has invalid conversation timestamps"));
    }
    if (head.exists && delta.metadata.workspace != head.workspace) {
        return lighter::outcome_error(Error::storage("session delta changes immutable workspace association"));
    }
    if (head.exists && delta.metadata.forked_from != head.forked_from) {
        return lighter::outcome_error(Error::storage("session delta changes immutable fork provenance"));
    }
    if (delta.metadata.preview.size() > 240 || !lighter::encoding::utf8::is_valid(delta.metadata.preview)) {
        return lighter::outcome_error(Error::storage("session delta has an invalid preview"));
    }
    if (delta.metadata.title && (delta.metadata.title->empty() || delta.metadata.title->size() > 200 ||
                                 !lighter::encoding::utf8::is_valid(*delta.metadata.title))) {
        return lighter::outcome_error(Error::storage("session delta has an invalid title"));
    }
    if (delta.metadata.model_preference &&
        (delta.metadata.model_preference->provider.empty() || delta.metadata.model_preference->model.empty())) {
        return lighter::outcome_error(Error::storage("session delta has an incomplete model preference"));
    }
    if (delta.metadata.forked_from && delta.metadata.forked_from->entry.value == 0) {
        return lighter::outcome_error(Error::storage("session delta has an invalid fork origin"));
    }
    if (delta.entry_count != head.entry_count + delta.entries.size() || delta.next_entry_id != delta.entry_count + 1) {
        return lighter::outcome_error(Error::storage("session delta does not extend the durable entry sequence"));
    }
    if (delta.active_leaf && (delta.active_leaf->value == 0 || delta.active_leaf->value > delta.entry_count)) {
        return lighter::outcome_error(Error::storage("session delta selects an unknown active leaf"));
    }
    if (delta.next_task_id < head.next_task_id || delta.next_provider_call_id < head.next_provider_call_id ||
        delta.tokens_used < head.tokens_used) {
        return lighter::outcome_error(Error::storage("session delta regresses durable counters"));
    }
    u64 maximum_task_id = 0;
    u64 maximum_provider_call_id = 0;
    for (usize index = 0; index < delta.entries.size(); ++index) {
        const auto &entry = delta.entries[index];
        if (entry.id.value != head.entry_count + index + 1 || entry.created_at_ms <= 0 ||
            (entry.parent_id && (entry.parent_id->value == 0 || entry.parent_id->value >= entry.id.value))) {
            return lighter::outcome_error(Error::storage("session delta contains an invalid entry envelope"));
        }
        std::visit(
            [&](const auto &payload) {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<T, TaskStarted> || std::same_as<T, TaskFinished>)
                    maximum_task_id = std::max(maximum_task_id, payload.id.value);
                else if constexpr (std::same_as<T, OutputItemCompleted> || std::same_as<T, ToolResults>) {
                    maximum_task_id = std::max(maximum_task_id, payload.task_id.value);
                    maximum_provider_call_id = std::max(maximum_provider_call_id, payload.provider_call_id.value);
                } else if constexpr (std::same_as<T, ProviderCallCompleted> || std::same_as<T, ProviderCallAborted>) {
                    maximum_task_id = std::max(maximum_task_id, payload.task_id.value);
                    maximum_provider_call_id = std::max(maximum_provider_call_id, payload.id.value);
                }
            },
            entry.payload);
    }
    if (delta.next_task_id <= maximum_task_id || delta.next_provider_call_id <= maximum_provider_call_id) {
        return lighter::outcome_error(Error::storage("session delta lifecycle counters do not cover its entries"));
    }
    return {};
}

Result<PreparedDelta> prepare_delta(const SessionDelta &delta, const DurableHead &head) {
    if (auto valid = validate_delta(delta, head); !valid) return lighter::outcome_error(std::move(valid).error());
    PreparedDelta prepared{.delta = delta};
    prepared.entries.reserve(delta.entries.size());
    for (const auto &entry : delta.entries) {
        auto encoded = encode_payload(entry.payload);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        prepared.entries.push_back(
            {.id = entry.id, .parent_id = entry.parent_id, .created_at_ms = entry.created_at_ms, .payload = *std::move(encoded)});
    }
    return prepared;
}

std::string marker_contents(u64 revision) { return "1 " + std::to_string(revision) + "\n"; }

Result<u64> read_marker(const std::filesystem::path &path) {
    auto reparse = detail::is_reparse_point(path);
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse) return lighter::outcome_error(Error::storage("catalog marker is a symlink, junction, or reparse point"));
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect catalog marker: " + error.message()));
    if (size == 0 || size > k_marker_maximum_size)
        return lighter::outcome_error(Error::storage("catalog marker has invalid bounded format"));
    std::ifstream input(path, std::ios::binary);
    if (!input) return lighter::outcome_error(Error::storage("cannot read catalog marker"));
    std::string contents(static_cast<usize>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input || !contents.starts_with("1 ") || !contents.ends_with('\n')) {
        return lighter::outcome_error(Error::storage("catalog marker has invalid format version"));
    }
    std::string_view revision_text(contents);
    revision_text.remove_prefix(2);
    revision_text.remove_suffix(1);
    u64 revision = 0;
    const auto parsed = std::from_chars(revision_text.data(), revision_text.data() + revision_text.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != revision_text.data() + revision_text.size() || revision == 0) {
        return lighter::outcome_error(Error::storage("catalog marker has an invalid target revision"));
    }
    return revision;
}

Result<void> write_marker(const StatePaths &paths, SessionId id, u64 revision) {
    std::error_code error;
    std::filesystem::create_directories(paths.catalog_pending(), error);
    if (error) return lighter::outcome_error(Error::storage("cannot create catalog-pending directory: " + error.message()));
    const auto path = paths.pending_marker(id);
    const auto exists = std::filesystem::exists(path, error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect catalog marker: " + error.message()));
    if (exists) {
        auto current = read_marker(path);
        if (!current) return lighter::outcome_error(std::move(current).error());
        if (*current >= revision) return {};
    }
    return detail::durable_replace_file(path, marker_contents(revision));
}

Result<void> remove_marker_if_satisfied(const StatePaths &paths, SessionId id, u64 revision) {
    auto current = read_marker(paths.pending_marker(id));
    if (!current) return lighter::outcome_error(std::move(current).error());
    if (*current > revision) return {};
    return detail::durable_remove_file(paths.pending_marker(id));
}

Result<CatalogProjection> read_projection(sqlite3 *database, SessionId expected) {
    auto query = prepare(database, "SELECT session_id,revision,workspace_key,updated_at_ms,title,preview FROM session WHERE singleton=1");
    if (!query) return lighter::outcome_error(std::move(query).error());
    const auto row = sqlite3_step(query->value);
    if (row == SQLITE_DONE) return lighter::outcome_error(Error::storage("session database has no singleton row"));
    if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(database, "cannot read session singleton", row));
    auto id = column_id(query->value, 0);
    if (!id) return lighter::outcome_error(std::move(id).error());
    if (*id != expected) return lighter::outcome_error(Error::storage("session database identity does not match its directory"));
    return CatalogProjection{
        .summary = {.id = *id,
                    .updated_at_ms = sqlite3_column_int64(query->value, 3),
                    .title = optional_text(query->value, 4),
                    .preview = text(query->value, 5)},
        .observed_revision = static_cast<u64>(sqlite3_column_int64(query->value, 1)),
        .workspace_key = text(query->value, 2),
    };
}

Result<CatalogProjection> read_published_projection(const StatePaths &paths, SessionId id) {
    std::error_code error;
    if (!std::filesystem::is_directory(paths.session_directory(id), error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect session directory: " + error.message()));
        return lighter::outcome_error(Error::storage("session was not found"));
    }
    if (!std::filesystem::is_regular_file(paths.session_database(id), error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect session database: " + error.message()));
        return lighter::outcome_error(Error::storage("published session database is absent"));
    }
    for (const auto &path : {paths.session_directory(id), paths.session_database(id)}) {
        auto reparse = detail::is_reparse_point(path);
        if (!reparse) return lighter::outcome_error(std::move(reparse).error());
        if (*reparse) return lighter::outcome_error(Error::storage("session authority is a symlink, junction, or reparse point"));
    }
    sqlite3 *database = nullptr;
    const auto encoded = path_utf8(paths.session_database(id));
    const auto code = sqlite3_open_v2(encoded.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const auto error_result = sqlite_error(database, "cannot open session database projection", code);
        if (database) sqlite3_close(database);
        return lighter::outcome_error(error_result);
    }
    if (auto configured = execute(database, "PRAGMA query_only=ON; PRAGMA busy_timeout=250;"); !configured) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(configured).error());
    }
    if (auto valid = validate_session_schema(database, false); !valid) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(valid).error());
    }
    auto projection = read_projection(database, id);
    sqlite3_close(database);
    return projection;
}

Result<CatalogProjection> upsert_projection(const StatePaths &paths, const SessionCatalog &catalog, SessionId id) {
    auto projection = read_published_projection(paths, id);
    if (!projection) return lighter::outcome_error(std::move(projection).error());
    if (auto projected = catalog.upsert(*projection); !projected) return lighter::outcome_error(std::move(projected).error());
    return projection;
}

Result<void> settle_projection_marker(const StatePaths &paths, SessionId id, u64 observed_revision) {
    std::error_code error;
    const auto marker_exists = std::filesystem::exists(paths.pending_marker(id), error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect pending catalog marker: " + error.message()));
    if (!marker_exists) return {};
    return remove_marker_if_satisfied(paths, id, observed_revision);
}

Result<void> refresh_projection(const StatePaths &paths, const SessionCatalog &catalog, SessionId id) {
    auto projection = upsert_projection(paths, catalog, id);
    if (!projection) return lighter::outcome_error(std::move(projection).error());
    return settle_projection_marker(paths, id, projection->observed_revision);
}

void bind_singleton(sqlite3_stmt *statement, const SessionDelta &delta, SessionId id, u64 revision) {
    const auto &metadata = delta.metadata;
    sqlite3_bind_int(statement, 1, 1);
    bind_id(statement, 2, id);
    sqlite3_bind_int64(statement, 3, metadata.created_at_ms);
    sqlite3_bind_int64(statement, 4, metadata.updated_at_ms);
    sqlite3_bind_text(statement, 5, metadata.workspace->root.data(), static_cast<int>(metadata.workspace->root.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, metadata.workspace->key.data(), static_cast<int>(metadata.workspace->key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, metadata.working_directory.data(), static_cast<int>(metadata.working_directory.size()),
                      SQLITE_TRANSIENT);
    bind_optional_text(statement, 8, metadata.title);
    sqlite3_bind_text(statement, 9, metadata.preview.data(), static_cast<int>(metadata.preview.size()), SQLITE_TRANSIENT);
    bind_optional_entry(statement, 10, delta.active_leaf);
    sqlite3_bind_int64(statement, 11, static_cast<sqlite3_int64>(delta.next_entry_id));
    sqlite3_bind_int64(statement, 12, static_cast<sqlite3_int64>(delta.next_task_id));
    sqlite3_bind_int64(statement, 13, static_cast<sqlite3_int64>(delta.next_provider_call_id));
    sqlite3_bind_int64(statement, 14, static_cast<sqlite3_int64>(delta.entry_count));
    sqlite3_bind_int64(statement, 15, static_cast<sqlite3_int64>(delta.tokens_used));
    bind_optional_text(statement, 16, metadata.model_preference ? std::optional{metadata.model_preference->provider} : std::nullopt);
    bind_optional_text(statement, 17, metadata.model_preference ? std::optional{metadata.model_preference->model} : std::nullopt);
    bind_optional_text(statement, 18, metadata.model_preference ? metadata.model_preference->reasoning_effort : std::nullopt);
    sqlite3_bind_int64(statement, 19, static_cast<sqlite3_int64>(revision));
    bind_optional_session(statement, 20, metadata.forked_from ? std::optional{metadata.forked_from->session} : std::nullopt);
    bind_optional_entry(statement, 21, metadata.forked_from ? std::optional{metadata.forked_from->entry} : std::nullopt);
}

Result<void> insert_entries(sqlite3 *database, const std::vector<EncodedEntry> &entries) {
    auto insert = prepare(database, R"sql(
INSERT INTO session_entries(entry_id,task_id,provider_call_id,parent_entry_id,kind,payload_version,payload_json,created_at_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8)
)sql");
    if (!insert) return lighter::outcome_error(std::move(insert).error());
    for (const auto &entry : entries) {
        sqlite3_reset(insert->value);
        sqlite3_clear_bindings(insert->value);
        sqlite3_bind_int64(insert->value, 1, static_cast<sqlite3_int64>(entry.id.value));
        if (entry.payload.task_id)
            sqlite3_bind_int64(insert->value, 2, static_cast<sqlite3_int64>(entry.payload.task_id->value));
        else
            sqlite3_bind_null(insert->value, 2);
        if (entry.payload.provider_call_id)
            sqlite3_bind_int64(insert->value, 3, static_cast<sqlite3_int64>(entry.payload.provider_call_id->value));
        else
            sqlite3_bind_null(insert->value, 3);
        bind_optional_entry(insert->value, 4, entry.parent_id);
        sqlite3_bind_int(insert->value, 5, static_cast<int>(entry.payload.kind));
        sqlite3_bind_int(insert->value, 6, static_cast<int>(entry.payload.version));
        sqlite3_bind_text(insert->value, 7, entry.payload.json.data(), static_cast<int>(entry.payload.json.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert->value, 8, entry.created_at_ms);
        const auto row = sqlite3_step(insert->value);
        if (row != SQLITE_DONE) return lighter::outcome_error(sqlite_error(database, "cannot append session entry", row));
    }
    return {};
}

DurableHead head_from_delta(const SessionDelta &delta, u64 revision) {
    return {.exists = true,
            .revision = revision,
            .entry_count = delta.entry_count,
            .next_task_id = delta.next_task_id,
            .next_provider_call_id = delta.next_provider_call_id,
            .tokens_used = delta.tokens_used,
            .created_at_ms = delta.metadata.created_at_ms,
            .updated_at_ms = delta.metadata.updated_at_ms,
            .workspace = delta.metadata.workspace,
            .forked_from = delta.metadata.forked_from,
            .title = delta.metadata.title,
            .preview = delta.metadata.preview};
}

std::string staging_nonce() {
    std::mt19937_64 random(std::random_device{}());
    std::array<char, 17> output{};
    std::to_chars(output.data(), output.data() + output.size() - 1, random(), 16);
    return output.data();
}

Result<void> create_state_directory(const std::filesystem::path &path) {
    std::error_code error;
    const auto created = std::filesystem::create_directories(path, error);
    if (error)
        return lighter::outcome_error(Error::storage("cannot create state directory '" + path.generic_string() + "': " + error.message()));
#ifndef _WIN32
    if (created) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
        if (error) return lighter::outcome_error(Error::storage("cannot secure state directory: " + error.message()));
    }
#endif
    auto reparse = detail::is_reparse_point(path);
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse)
        return lighter::outcome_error(Error::storage("state directory is a symlink, junction, or reparse point: " + path.generic_string()));
    return {};
}

Result<SessionId> canonical_path_id(const std::filesystem::path &path) {
    const auto name = path.filename().string();
    auto id = parse_session_id(name);
    if (!id || to_string(*id) != name) return lighter::outcome_error(Error::storage("state entry name is not a canonical session ID"));
    return *id;
}

Result<SessionId> staging_path_id(const std::filesystem::path &path) {
    const auto name = path.filename().string();
    const auto separator = name.find('.');
    if (separator == std::string::npos || separator + 1 == name.size())
        return lighter::outcome_error(Error::storage("staging entry name is invalid"));
    auto id = parse_session_id(std::string_view(name).substr(0, separator));
    if (!id || to_string(*id) != std::string_view(name).substr(0, separator) ||
        !std::ranges::all_of(std::string_view(name).substr(separator + 1),
                             [](char character) { return std::isxdigit(static_cast<unsigned char>(character)); })) {
        return lighter::outcome_error(Error::storage("staging entry name is invalid"));
    }
    return *id;
}

Result<void> remove_staging_tree(const std::filesystem::path &path) {
    auto reparse = detail::is_reparse_point(path);
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse) return lighter::outcome_error(Error::storage("staging entry is a symlink, junction, or reparse point"));
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect staging entry: " + error.message()));
        return lighter::outcome_error(Error::storage("staging entry is not a directory"));
    }
    for (std::filesystem::recursive_directory_iterator iterator(path, error), end; !error && iterator != end; iterator.increment(error)) {
        auto child_reparse = detail::is_reparse_point(iterator->path());
        if (!child_reparse) return lighter::outcome_error(std::move(child_reparse).error());
        if (*child_reparse) return lighter::outcome_error(Error::storage("staging tree contains a reparse point"));
    }
    if (error) return lighter::outcome_error(Error::storage("cannot inspect staging tree: " + error.message()));
    std::filesystem::remove_all(path, error);
    if (error) return lighter::outcome_error(Error::storage("cannot remove abandoned staging directory: " + error.message()));
    return {};
}

} // namespace

struct CatalogInvalidationOwner {
    explicit CatalogInvalidationOwner(SessionLease lease) : lease(std::move(lease)) {}

    SessionLease lease;
    std::mutex mutex;
    mutable std::mutex status_mutex;
    CatalogRefreshStatus status;
};

struct StagedBaseline {
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
    std::optional<SessionWorkspace> workspace;
    std::optional<ForkOrigin> forked_from;
    u64 next_entry_id = 0;
    u64 next_task_id = 0;
    u64 next_provider_call_id = 0;
    u64 entry_count = 0;
    u64 tokens_used = 0;
};

struct SessionWriter::State {
    State(std::shared_ptr<SessionRepository::State> repository, SessionLease lease, SessionId id)
        : repository(std::move(repository)), invalidation(std::make_shared<CatalogInvalidationOwner>(std::move(lease))), id(id) {}
    ~State() {
        if (database) sqlite3_close(database);
        if (!published && !staging_directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(staging_directory, error);
        }
    }
    std::shared_ptr<SessionRepository::State> repository;
    std::shared_ptr<CatalogInvalidationOwner> invalidation;
    SessionId id;
    sqlite3 *database = nullptr;
    std::filesystem::path staging_directory;
    DurableHead head;
    std::vector<EncodedEntry> staged_entries;
    std::optional<StagedBaseline> staged_baseline;
    bool published = false;
    bool staged = false;
    std::mutex mutex;
};

namespace {

void set_catalog_status(CatalogInvalidationOwner &owner, std::optional<std::string> degradation) {
    std::scoped_lock lock(owner.status_mutex);
    owner.status.degraded = degradation.has_value();
    owner.status.detail = degradation.value_or("");
}

} // namespace

SessionWriter::~SessionWriter() = default;
SessionWriter::SessionWriter(SessionWriter &&) noexcept = default;
SessionWriter &SessionWriter::operator=(SessionWriter &&) noexcept = default;
SessionId SessionWriter::session_id() const noexcept { return state->id; }

namespace {

Result<void> stage_prepared(SessionWriter::State &state, const PreparedDelta &prepared) {
    const StatePaths paths{state.repository->root};
    std::error_code error;
    std::filesystem::create_directories(paths.staging(), error);
    if (error) return lighter::outcome_error(Error::storage("cannot create session staging directory: " + error.message()));
    state.staging_directory = paths.staging() / (to_string(state.id) + "." + staging_nonce());
    if (!std::filesystem::create_directory(state.staging_directory, error) || error) {
        return lighter::outcome_error(Error::storage("cannot create unique session staging directory: " + error.message()));
    }
    auto opened = open_database(state.staging_directory / "session.sqlite3", true, false);
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    sqlite3 *database = *opened;
    auto transaction = execute(database, "BEGIN IMMEDIATE");
    if (!transaction) {
        sqlite3_close(database);
        return transaction;
    }
    auto insert = prepare(database, R"sql(
INSERT INTO session(singleton,session_id,created_at_ms,updated_at_ms,workspace_root,workspace_key,working_directory,title,preview,
 active_leaf_entry_id,next_entry_id,next_task_id,next_provider_call_id,entry_count,tokens_used,last_provider,last_model,
 last_reasoning_effort,revision,forked_from_session_id,forked_from_entry_id)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)
)sql");
    if (!insert) {
        static_cast<void>(execute(database, "ROLLBACK"));
        sqlite3_close(database);
        return lighter::outcome_error(std::move(insert).error());
    }
    bind_singleton(insert->value, prepared.delta, state.id, 1);
    if (sqlite3_step(insert->value) != SQLITE_DONE) {
        const auto error_result = sqlite_error(database, "cannot create session singleton");
        static_cast<void>(execute(database, "ROLLBACK"));
        sqlite3_close(database);
        return lighter::outcome_error(error_result);
    }
    if (auto entries = insert_entries(database, prepared.entries); !entries) {
        static_cast<void>(execute(database, "ROLLBACK"));
        sqlite3_close(database);
        return entries;
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        static_cast<void>(execute(database, "ROLLBACK"));
        sqlite3_close(database);
        return committed;
    }
    sqlite3_finalize(insert->value);
    insert->value = nullptr;
    if (sqlite3_close(database) != SQLITE_OK) return lighter::outcome_error(Error::storage("cannot close staged session database"));
    state.head = head_from_delta(prepared.delta, 1);
    state.staged_entries = prepared.entries;
    state.staged_baseline = StagedBaseline{
        .created_at_ms = prepared.delta.metadata.created_at_ms,
        .updated_at_ms = prepared.delta.metadata.updated_at_ms,
        .workspace = prepared.delta.metadata.workspace,
        .forked_from = prepared.delta.metadata.forked_from,
        .next_entry_id = prepared.delta.next_entry_id,
        .next_task_id = prepared.delta.next_task_id,
        .next_provider_call_id = prepared.delta.next_provider_call_id,
        .entry_count = prepared.delta.entry_count,
        .tokens_used = prepared.delta.tokens_used,
    };
    state.staged = true;
    return {};
}

bool same_entry(const EncodedEntry &left, const EncodedEntry &right) {
    return left.id == right.id && left.parent_id == right.parent_id && left.created_at_ms == right.created_at_ms &&
           left.payload.kind == right.payload.kind && left.payload.version == right.payload.version &&
           left.payload.json == right.payload.json && left.payload.task_id == right.payload.task_id &&
           left.payload.provider_call_id == right.payload.provider_call_id;
}

Result<void> validate_staged_final(const SessionWriter::State &state, const SessionDelta &delta) {
    auto prepared = prepare_delta(delta, {});
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    if (!state.staged_baseline) return lighter::outcome_error(Error::storage("staged publication has no retained baseline"));
    const auto &initial = *state.staged_baseline;
    if (prepared->entries.size() != state.staged_entries.size() ||
        !std::ranges::equal(prepared->entries, state.staged_entries, same_entry)) {
        return lighter::outcome_error(Error::storage("staged publication snapshot changed its authoritative semantic prefix"));
    }
    if (delta.metadata.created_at_ms != initial.created_at_ms || delta.metadata.workspace != initial.workspace ||
        delta.metadata.forked_from != initial.forked_from) {
        return lighter::outcome_error(Error::storage("staged publication snapshot changed immutable session metadata"));
    }
    if (delta.metadata.updated_at_ms < initial.updated_at_ms || delta.next_entry_id < initial.next_entry_id ||
        delta.next_task_id < initial.next_task_id || delta.next_provider_call_id < initial.next_provider_call_id ||
        delta.entry_count < initial.entry_count || delta.tokens_used < initial.tokens_used) {
        return lighter::outcome_error(Error::storage("staged publication snapshot regresses its retained baseline"));
    }
    return {};
}

Result<void> update_staged_singleton(SessionWriter::State &state, const SessionDelta &delta) {
    auto opened = open_database(state.staging_directory / "session.sqlite3", false, false);
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    sqlite3 *database = *opened;
    auto update = prepare(database, R"sql(
UPDATE session SET created_at_ms=?3,updated_at_ms=?4,workspace_root=?5,workspace_key=?6,working_directory=?7,title=?8,
 preview=?9,active_leaf_entry_id=?10,next_entry_id=?11,next_task_id=?12,next_provider_call_id=?13,entry_count=?14,
 tokens_used=?15,last_provider=?16,last_model=?17,last_reasoning_effort=?18,revision=?19,
 forked_from_session_id=?20,forked_from_entry_id=?21 WHERE singleton=?1 AND session_id=?2
)sql");
    if (!update) {
        sqlite3_close(database);
        return lighter::outcome_error(std::move(update).error());
    }
    bind_singleton(update->value, delta, state.id, 1);
    const auto row = sqlite3_step(update->value);
    if (row != SQLITE_DONE || sqlite3_changes(database) != 1) {
        const auto error = sqlite_error(database, "cannot finalize staged session singleton", row);
        sqlite3_close(database);
        return lighter::outcome_error(error);
    }
    sqlite3_finalize(update->value);
    update->value = nullptr;
    if (sqlite3_close(database) != SQLITE_OK) return lighter::outcome_error(Error::storage("cannot close finalized staging database"));
    state.head = head_from_delta(delta, 1);
    return {};
}

struct PublishedAuthority {
    CatalogProjection projection;
    std::optional<std::string> degradation;
};

Result<PublishedAuthority> publish_staged(SessionWriter::State &state, const SessionDelta &delta) {
    if (auto valid = validate_staged_final(state, delta); !valid) return lighter::outcome_error(std::move(valid).error());
    if (auto updated = update_staged_singleton(state, delta); !updated) return lighter::outcome_error(std::move(updated).error());
    const StatePaths paths{state.repository->root};
    std::optional<std::string> degradation;
    {
        std::scoped_lock marker_lock(state.invalidation->mutex);
        if (auto marker = write_marker(paths, state.id, 1); !marker) degradation = marker.error().message();
    }
    if (auto published = detail::publish_directory_without_replacement(state.staging_directory, paths.session_directory(state.id));
        !published) {
        return lighter::outcome_error(std::move(published).error());
    }
    state.staging_directory.clear();
    state.published = true;
    state.staged = false;
    state.staged_entries.clear();
    state.staged_entries.shrink_to_fit();
    state.staged_baseline.reset();
    auto opened = open_database(paths.session_database(state.id), false, true);
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    state.database = *opened;
    auto projection = read_projection(state.database, state.id);
    if (!projection) return lighter::outcome_error(std::move(projection).error());
    return PublishedAuthority{.projection = *std::move(projection), .degradation = std::move(degradation)};
}

SessionCommitResult project_publication(SessionWriter::State &state, PublishedAuthority published) {
    const StatePaths paths{state.repository->root};
    auto projection = upsert_projection(paths, state.repository->catalog, state.id);
    if (!projection) {
        published.degradation = projection.error().message();
    } else {
        std::scoped_lock marker_lock(state.invalidation->mutex);
        if (auto settled = settle_projection_marker(paths, state.id, projection->observed_revision); !settled)
            published.degradation = settled.error().message();
    }
    set_catalog_status(*state.invalidation, published.degradation);
    return {.catalog_degradation = std::move(published.degradation)};
}

} // namespace

struct CatalogIndexer {
    struct Request {
        SessionId id;
        std::weak_ptr<CatalogInvalidationOwner> owner;
        usize failures = 0;
        std::chrono::steady_clock::time_point not_before;
    };

    CatalogIndexer(std::filesystem::path root, SessionCatalog catalog, const StorageHookSlot &storage_hook)
        : paths{std::move(root)}, catalog(std::move(catalog)), storage_hook(storage_hook),
          worker([this](std::stop_token stop) { run(stop); }) {}

    ~CatalogIndexer() {
        worker.request_stop();
        changed.notify_all();
    }

    void enqueue(SessionId id, const std::shared_ptr<CatalogInvalidationOwner> &owner) {
        {
            std::scoped_lock lock(mutex);
            if (!queued.insert(id).second) return;
            requests.push_back({.id = id, .owner = owner, .not_before = std::chrono::steady_clock::now()});
        }
        changed.notify_all();
    }

    Result<void> refresh_live(SessionId id, CatalogInvalidationOwner &owner) {
        auto projection = upsert_projection(paths, catalog, id);
        if (!projection) {
            set_catalog_status(owner, projection.error().message());
            return lighter::outcome_error(std::move(projection).error());
        }
        Result<void> settled;
        {
            std::scoped_lock marker_lock(owner.mutex);
            settled = settle_projection_marker(paths, id, projection->observed_revision);
        }
        set_catalog_status(owner, settled ? std::nullopt : std::optional{settled.error().message()});
        return settled;
    }

private:
    Result<void> refresh(const Request &request) {
        notify_storage_test_hook(storage_hook, testing::StorageEvent::CATALOG_INDEXER_BEFORE_REFRESH);
        Result<void> result;
        if (auto owner = request.owner.lock()) {
            result = refresh_live(request.id, *owner);
        } else {
            auto lease = acquire_session_lease(paths.root, request.id);
            result = lease ? refresh_projection(paths, catalog, request.id) : lighter::outcome_error(std::move(lease).error());
        }
        notify_storage_test_hook(storage_hook, testing::StorageEvent::CATALOG_INDEXER_AFTER_REFRESH);
        return result;
    }

    void run(std::stop_token stop) {
        using namespace std::chrono_literals;
        while (!stop.stop_requested()) {
            Request request;
            {
                std::unique_lock lock(mutex);
                while (true) {
                    changed.wait(lock, stop, [this] { return !requests.empty(); });
                    if (stop.stop_requested()) return;
                    const auto now = std::chrono::steady_clock::now();
                    auto ready = std::ranges::find_if(requests, [now](const Request &candidate) { return candidate.not_before <= now; });
                    if (ready != requests.end()) {
                        request = std::move(*ready);
                        requests.erase(ready);
                        queued.erase(request.id);
                        break;
                    }
                    const auto earliest = std::ranges::min_element(requests, {}, &Request::not_before)->not_before;
                    changed.wait_until(lock, stop, earliest, [this, earliest] {
                        return std::ranges::any_of(requests,
                                                   [earliest](const Request &candidate) { return candidate.not_before < earliest; });
                    });
                    if (stop.stop_requested()) return;
                }
            }
            if (auto refreshed = refresh(request); refreshed) continue;
            ++request.failures;
            const auto delay = request.failures == 1 ? 100ms : request.failures == 2 ? 300ms : request.failures == 3 ? 1s : 5s;
            request.not_before = std::chrono::steady_clock::now() + delay;
            {
                std::scoped_lock lock(mutex);
                if (queued.insert(request.id).second) requests.push_back(std::move(request));
            }
            changed.notify_all();
        }
    }

    StatePaths paths;
    SessionCatalog catalog;
    const StorageHookSlot &storage_hook;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<Request> requests;
    std::set<SessionId> queued;
    std::jthread worker;
};

Result<Session> SessionWriter::load() {
    std::scoped_lock lock(state->mutex);
    if (!state->published || !state->database) return lighter::outcome_error(Error::storage("session is not published"));
    auto singleton = prepare(state->database, R"sql(
SELECT session_id,created_at_ms,updated_at_ms,workspace_root,workspace_key,working_directory,title,preview,
 active_leaf_entry_id,next_entry_id,next_task_id,next_provider_call_id,last_provider,last_model,last_reasoning_effort,
 revision,forked_from_session_id,forked_from_entry_id,entry_count,tokens_used
FROM session WHERE singleton=1
)sql");
    if (!singleton) return lighter::outcome_error(std::move(singleton).error());
    const auto row = sqlite3_step(singleton->value);
    if (row == SQLITE_DONE) return lighter::outcome_error(Error::storage("session database has no singleton row"));
    if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot load session singleton", row));
    auto durable_id = column_id(singleton->value, 0);
    if (!durable_id) return lighter::outcome_error(std::move(durable_id).error());
    if (*durable_id != state->id)
        return lighter::outcome_error(Error::storage("session database identity does not match requested session"));
    Session session(state->id);
    session.metadata.created_at_ms = sqlite3_column_int64(singleton->value, 1);
    session.metadata.updated_at_ms = sqlite3_column_int64(singleton->value, 2);
    session.metadata.workspace = SessionWorkspace{.root = text(singleton->value, 3), .key = text(singleton->value, 4)};
    session.metadata.working_directory = text(singleton->value, 5);
    session.metadata.title = optional_text(singleton->value, 6);
    session.metadata.preview = text(singleton->value, 7);
    if (sqlite3_column_type(singleton->value, 8) != SQLITE_NULL)
        session.active_leaf = EntryId{static_cast<u64>(sqlite3_column_int64(singleton->value, 8))};
    session.next_entry_id = static_cast<u64>(sqlite3_column_int64(singleton->value, 9));
    session.next_task_id = static_cast<u64>(sqlite3_column_int64(singleton->value, 10));
    session.next_provider_call_id = static_cast<u64>(sqlite3_column_int64(singleton->value, 11));
    auto provider = optional_text(singleton->value, 12);
    auto model = optional_text(singleton->value, 13);
    auto reasoning = optional_text(singleton->value, 14);
    if (provider.has_value() != model.has_value() || (reasoning && !provider))
        return lighter::outcome_error(Error::storage("durable session has incomplete model preference"));
    if (provider)
        session.metadata.model_preference =
            SessionModelPreference{.provider = *std::move(provider), .model = *std::move(model), .reasoning_effort = std::move(reasoning)};
    const auto revision = static_cast<u64>(sqlite3_column_int64(singleton->value, 15));
    std::optional<SessionId> fork_session;
    if (sqlite3_column_type(singleton->value, 16) != SQLITE_NULL) {
        auto id = column_id(singleton->value, 16);
        if (!id) return lighter::outcome_error(std::move(id).error());
        fork_session = *id;
    }
    std::optional<EntryId> fork_entry;
    if (sqlite3_column_type(singleton->value, 17) != SQLITE_NULL)
        fork_entry = EntryId{static_cast<u64>(sqlite3_column_int64(singleton->value, 17))};
    if (fork_session.has_value() != fork_entry.has_value())
        return lighter::outcome_error(Error::storage("durable session has incomplete fork origin"));
    if (fork_session) session.metadata.forked_from = ForkOrigin{.session = *fork_session, .entry = *fork_entry};
    const auto entry_count = static_cast<u64>(sqlite3_column_int64(singleton->value, 18));
    const auto tokens = static_cast<u64>(sqlite3_column_int64(singleton->value, 19));

    auto entries = prepare(state->database, R"sql(
SELECT entry_id,task_id,provider_call_id,parent_entry_id,kind,payload_version,payload_json,created_at_ms
FROM session_entries ORDER BY entry_id
)sql");
    if (!entries) return lighter::outcome_error(std::move(entries).error());
    while (true) {
        const auto entry_row = sqlite3_step(entries->value);
        if (entry_row == SQLITE_DONE) break;
        if (entry_row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot load session entries", entry_row));
        const auto kind = static_cast<EntryKind>(sqlite3_column_int(entries->value, 4));
        const auto version = static_cast<u32>(sqlite3_column_int(entries->value, 5));
        auto payload = decode_payload(kind, version, text(entries->value, 6));
        if (!payload) return lighter::outcome_error(std::move(payload).error());
        auto encoded = encode_payload(*payload);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        const auto task = sqlite3_column_type(entries->value, 1) == SQLITE_NULL ?
                              std::optional<u64>{} :
                              std::optional<u64>{static_cast<u64>(sqlite3_column_int64(entries->value, 1))};
        const auto call = sqlite3_column_type(entries->value, 2) == SQLITE_NULL ?
                              std::optional<u64>{} :
                              std::optional<u64>{static_cast<u64>(sqlite3_column_int64(entries->value, 2))};
        if (task != (encoded->task_id ? std::optional<u64>{encoded->task_id->value} : std::nullopt) ||
            call != (encoded->provider_call_id ? std::optional<u64>{encoded->provider_call_id->value} : std::nullopt)) {
            return lighter::outcome_error(Error::storage("session entry lifecycle index does not match its payload"));
        }
        SessionEntry entry{.id = {static_cast<u64>(sqlite3_column_int64(entries->value, 0))},
                           .payload = *std::move(payload),
                           .created_at_ms = sqlite3_column_int64(entries->value, 7)};
        if (sqlite3_column_type(entries->value, 3) != SQLITE_NULL)
            entry.parent_id = EntryId{static_cast<u64>(sqlite3_column_int64(entries->value, 3))};
        session.entries.push_back(std::move(entry));
    }
    if (session.entries.size() != entry_count || session.tokens_used() != tokens)
        return lighter::outcome_error(Error::storage("session singleton counters do not match history"));
    if (auto valid = session.validate(); !valid)
        return lighter::outcome_error(Error::storage("invalid durable session: " + valid.error().detail));
    state->head = head_from_delta(make_delta(session, {}), revision);
    return session;
}

Result<void> SessionWriter::stage_initial(const SessionDelta &delta) {
    DurableHead head;
    auto prepared = prepare_delta(delta, head);
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    std::scoped_lock lock(state->mutex);
    if (state->published || state->staged) return lighter::outcome_error(Error::storage("session writer is already materialized"));
    return stage_prepared(*state, *prepared);
}

Result<CatalogProjection> SessionWriter::projection() const {
    std::scoped_lock lock(state->mutex);
    if (!state->published || !state->database) return lighter::outcome_error(Error::storage("session is not published"));
    return read_projection(state->database, state->id);
}

Result<SessionCommitResult> SessionWriter::commit(const SessionDelta &delta) {
    DurableHead head;
    bool staged = false;
    {
        std::scoped_lock lock(state->mutex);
        head = state->head;
        staged = state->staged;
    }
    if (staged) {
        std::unique_lock lock(state->mutex);
        auto published = publish_staged(*state, delta);
        if (!published) return lighter::outcome_error(std::move(published).error());
        lock.unlock();
        auto result = project_publication(*state, *std::move(published));
        if (result.catalog_degradation) state->repository->indexer->enqueue(state->id, state->invalidation);
        return result;
    }
    auto prepared = prepare_delta(delta, head);
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    std::unique_lock lock(state->mutex);
    if (!state->published) {
        if (auto staged_result = stage_prepared(*state, *prepared); !staged_result)
            return lighter::outcome_error(std::move(staged_result).error());
        auto published = publish_staged(*state, delta);
        if (!published) return lighter::outcome_error(std::move(published).error());
        lock.unlock();
        auto result = project_publication(*state, *std::move(published));
        if (result.catalog_degradation) state->repository->indexer->enqueue(state->id, state->invalidation);
        return result;
    }
    if (head.revision != state->head.revision) return lighter::outcome_error(Error::storage("session writer received concurrent commits"));
    const auto target_revision = state->head.revision + 1;
    const bool title_changed = delta.metadata.title != state->head.title;
    const bool catalog_visible = delta.metadata.updated_at_ms != state->head.updated_at_ms || delta.metadata.title != state->head.title ||
                                 delta.metadata.preview != state->head.preview;
    const StatePaths paths{state->repository->root};
    std::optional<std::string> degradation;
    bool projection_pending = false;
    {
        std::scoped_lock marker_lock(state->invalidation->mutex);
        std::error_code marker_error;
        projection_pending = std::filesystem::exists(paths.pending_marker(state->id), marker_error);
        if (marker_error && !degradation) degradation = "cannot inspect pending catalog marker: " + marker_error.message();
        if (catalog_visible || projection_pending) {
            if (auto marker = write_marker(paths, state->id, target_revision); !marker) degradation = marker.error().message();
        }
    }
    if (auto begun = execute(state->database, "BEGIN IMMEDIATE"); !begun) return lighter::outcome_error(std::move(begun).error());
    auto revision = prepare(state->database, "SELECT revision FROM session WHERE singleton=1");
    if (!revision || sqlite3_step(revision->value) != SQLITE_ROW ||
        static_cast<u64>(sqlite3_column_int64(revision->value, 0)) != state->head.revision) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(Error::storage("session revision conflict"));
    }
    if (auto entries = insert_entries(state->database, prepared->entries); !entries) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(std::move(entries).error());
    }
    auto update = prepare(state->database, R"sql(
UPDATE session SET updated_at_ms=?2,working_directory=?3,title=?4,preview=?5,active_leaf_entry_id=?6,next_entry_id=?7,
 next_task_id=?8,next_provider_call_id=?9,entry_count=?10,tokens_used=?11,last_provider=?12,last_model=?13,
 last_reasoning_effort=?14,revision=?15,forked_from_session_id=?16,forked_from_entry_id=?17
WHERE singleton=1 AND revision=?1
)sql");
    if (!update) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(std::move(update).error());
    }
    sqlite3_bind_int64(update->value, 1, static_cast<sqlite3_int64>(state->head.revision));
    sqlite3_bind_int64(update->value, 2, delta.metadata.updated_at_ms);
    sqlite3_bind_text(update->value, 3, delta.metadata.working_directory.data(), static_cast<int>(delta.metadata.working_directory.size()),
                      SQLITE_TRANSIENT);
    bind_optional_text(update->value, 4, delta.metadata.title);
    sqlite3_bind_text(update->value, 5, delta.metadata.preview.data(), static_cast<int>(delta.metadata.preview.size()), SQLITE_TRANSIENT);
    bind_optional_entry(update->value, 6, delta.active_leaf);
    sqlite3_bind_int64(update->value, 7, static_cast<sqlite3_int64>(delta.next_entry_id));
    sqlite3_bind_int64(update->value, 8, static_cast<sqlite3_int64>(delta.next_task_id));
    sqlite3_bind_int64(update->value, 9, static_cast<sqlite3_int64>(delta.next_provider_call_id));
    sqlite3_bind_int64(update->value, 10, static_cast<sqlite3_int64>(delta.entry_count));
    sqlite3_bind_int64(update->value, 11, static_cast<sqlite3_int64>(delta.tokens_used));
    bind_optional_text(update->value, 12,
                       delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->provider} : std::nullopt);
    bind_optional_text(update->value, 13,
                       delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->model} : std::nullopt);
    bind_optional_text(update->value, 14,
                       delta.metadata.model_preference ? delta.metadata.model_preference->reasoning_effort : std::nullopt);
    sqlite3_bind_int64(update->value, 15, static_cast<sqlite3_int64>(target_revision));
    bind_optional_session(update->value, 16,
                          delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->session} : std::nullopt);
    bind_optional_entry(update->value, 17, delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->entry} : std::nullopt);
    const auto updated = sqlite3_step(update->value);
    if (updated != SQLITE_DONE || sqlite3_changes(state->database) != 1) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(sqlite_error(state->database, "cannot update session singleton", updated));
    }
    if (auto committed = execute(state->database, "COMMIT"); !committed) {
        static_cast<void>(execute(state->database, "ROLLBACK"));
        return lighter::outcome_error(std::move(committed).error());
    }
    state->head = head_from_delta(delta, target_revision);
    notify_storage_test_hook(state->repository->storage_hook, testing::StorageEvent::AUTHORITATIVE_COMMIT_COMPLETED);
    lock.unlock();
    if (catalog_visible || projection_pending) {
        if (title_changed) {
            if (auto projected = state->repository->indexer->refresh_live(state->id, *state->invalidation); !projected) {
                degradation = projected.error().message();
                state->repository->indexer->enqueue(state->id, state->invalidation);
            }
        } else {
            if (degradation) set_catalog_status(*state->invalidation, degradation);
            state->repository->indexer->enqueue(state->id, state->invalidation);
        }
    }
    return SessionCommitResult{.catalog_degradation = std::move(degradation)};
}

Result<void> SessionWriter::refresh_catalog() {
    auto refreshed = state->repository->indexer->refresh_live(state->id, *state->invalidation);
    if (!refreshed) state->repository->indexer->enqueue(state->id, state->invalidation);
    return refreshed;
}

CatalogRefreshStatus SessionWriter::catalog_status() const {
    std::scoped_lock lock(state->invalidation->status_mutex);
    return state->invalidation->status;
}

void testing::StorageHookAccess::set(SessionRepository &repository, StorageHook hook) {
    repository.state->storage_hook.set(std::move(hook));
}

SessionDelta make_delta(const Session &session, std::span<const SessionEntry> entries) {
    return {.entries = {entries.begin(), entries.end()},
            .active_leaf = session.active_leaf,
            .next_entry_id = session.next_entry_id,
            .next_task_id = session.next_task_id,
            .next_provider_call_id = session.next_provider_call_id,
            .entry_count = session.entries.size(),
            .tokens_used = session.tokens_used(),
            .metadata = session.metadata};
}

namespace {

Result<CatalogReconciliation> cleanup_abandoned_staging(const StatePaths &paths) {
    CatalogReconciliation result;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(paths.staging(), iteration_error), end; !iteration_error && iterator != end;
         iterator.increment(iteration_error)) {
        auto id = staging_path_id(iterator->path());
        if (!id) {
            result.warnings.push_back(id.error().message() + ": " + iterator->path().filename().string());
            continue;
        }
        auto lease = acquire_session_lease(paths.root, *id);
        if (!lease) {
            if (lease.error().detail.contains("in use by another"))
                ++result.busy;
            else
                result.warnings.push_back("cannot lease staging session " + to_string(*id) + ": " + lease.error().message());
            continue;
        }
        if (auto removed = remove_staging_tree(iterator->path()); !removed)
            result.warnings.push_back("cannot clean staging session " + to_string(*id) + ": " + removed.error().message());
        else
            ++result.repaired;
    }
    if (iteration_error)
        return lighter::outcome_error(Error::storage("cannot enumerate session staging directory: " + iteration_error.message()));
    return result;
}

Result<bool> catalog_rebuild_pending(const StatePaths &paths) {
    const auto marker = paths.catalog_rebuild_marker();
    std::error_code error;
    const auto exists = std::filesystem::exists(marker, error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect catalog rebuild marker: " + error.message()));
    if (!exists) return false;
    auto reparse = detail::is_reparse_point(marker);
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse) return lighter::outcome_error(Error::storage("catalog rebuild marker is a symlink, junction, or reparse point"));
    if (!std::filesystem::is_regular_file(marker, error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect catalog rebuild marker: " + error.message()));
        return lighter::outcome_error(Error::storage("catalog rebuild marker is not a regular file"));
    }
    std::ifstream input(marker, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input || contents != "1\n") return lighter::outcome_error(Error::storage("catalog rebuild marker has invalid format"));
    return true;
}

} // namespace

Result<SessionRepository> SessionRepository::open(std::filesystem::path state_root) {
    auto catalog = SessionCatalog::open(state_root);
    if (!catalog) return lighter::outcome_error(std::move(catalog).error());
    return open_with_catalog(std::move(state_root), *std::move(catalog));
}

Result<SessionRepository> SessionRepository::repair_catalog(std::filesystem::path state_root) {
    auto catalog = SessionCatalog::repair_corrupt(state_root);
    if (!catalog) return lighter::outcome_error(std::move(catalog).error());
    return open_with_catalog(std::move(state_root), *std::move(catalog));
}

Result<SessionRepository> SessionRepository::open_with_catalog(std::filesystem::path state_root, SessionCatalog catalog) {
    const StatePaths paths{state_root};
    if (auto created = create_state_directory(paths.root); !created) return lighter::outcome_error(std::move(created).error());
    for (const auto &directory : {paths.sessions(), paths.staging(), paths.catalog_pending(), paths.locks()}) {
        if (auto created = create_state_directory(directory); !created) return lighter::outcome_error(std::move(created).error());
    }
    auto repository_state = std::make_shared<State>(std::move(state_root), std::move(catalog));
    SessionRepository repository(std::move(repository_state));
    repository.state->indexer =
        std::make_shared<CatalogIndexer>(repository.state->root, repository.state->catalog, repository.state->storage_hook);
    auto staging = cleanup_abandoned_staging(paths);
    if (!staging) return lighter::outcome_error(std::move(staging).error());
    repository.state->warnings.insert(repository.state->warnings.end(), std::make_move_iterator(staging->warnings.begin()),
                                      std::make_move_iterator(staging->warnings.end()));
    auto rebuild_pending = catalog_rebuild_pending(paths);
    if (!rebuild_pending) return lighter::outcome_error(std::move(rebuild_pending).error());
    if (*rebuild_pending) {
        if (!repository.state->catalog.owns_rebuild_exclusivity()) {
            return lighter::outcome_error(Error::storage("catalog rebuild does not own exclusive maintenance"));
        }
        auto rebuilt = repository.rebuild_catalog();
        if (!rebuilt) return lighter::outcome_error(std::move(rebuilt).error());
        if (auto completed = detail::durable_remove_file(paths.catalog_rebuild_marker()); !completed)
            return lighter::outcome_error(Error::storage("cannot complete catalog rebuild: " + completed.error().message()));
        if (auto completed = repository.state->catalog.complete_rebuild(); !completed)
            return lighter::outcome_error(std::move(completed).error());
        repository.state->warnings.insert(repository.state->warnings.end(), std::make_move_iterator(rebuilt->warnings.begin()),
                                          std::make_move_iterator(rebuilt->warnings.end()));
    }
    auto reconciled = repository.reconcile_pending();
    if (!reconciled) return lighter::outcome_error(std::move(reconciled).error());
    repository.state->warnings.insert(repository.state->warnings.end(), std::make_move_iterator(reconciled->warnings.begin()),
                                      std::make_move_iterator(reconciled->warnings.end()));
    return repository;
}

const std::filesystem::path &SessionRepository::root() const noexcept { return state->root; }
const SessionCatalog &SessionRepository::catalog() const noexcept { return state->catalog; }
const std::vector<std::string> &SessionRepository::warnings() const noexcept { return state->warnings; }

Result<SessionWriter> SessionRepository::create(SessionId id) const {
    const StatePaths paths{state->root};
    std::error_code error;
    if (std::filesystem::exists(paths.session_directory(id), error))
        return lighter::outcome_error(Error::storage("session identity is already published"));
    if (error) return lighter::outcome_error(Error::storage("cannot inspect target session path: " + error.message()));
    auto lease = acquire_session_lease(state->root, id);
    if (!lease) return lighter::outcome_error(std::move(lease).error());
    return SessionWriter(std::make_shared<SessionWriter::State>(state, *std::move(lease), id));
}

Result<SessionWriter> SessionRepository::stage(SessionId id, const SessionDelta &initial) const {
    auto writer = create(id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    if (auto staged = writer->stage_initial(initial); !staged) return lighter::outcome_error(std::move(staged).error());
    return *std::move(writer);
}

Result<SessionWriter> SessionRepository::acquire(SessionId id) const {
    const StatePaths paths{state->root};
    auto lease = acquire_session_lease(state->root, id);
    if (!lease) return lighter::outcome_error(std::move(lease).error());
    std::error_code error;
    const auto directory_exists = std::filesystem::exists(paths.session_directory(id), error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect session directory: " + error.message()));
    if (!directory_exists) return lighter::outcome_error(Error::storage("session was not found", ErrorCode::NOT_FOUND));
    if (!std::filesystem::is_directory(paths.session_directory(id), error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect session directory: " + error.message()));
        return lighter::outcome_error(Error::storage("session authority is not a directory"));
    }
    auto reparse = detail::is_reparse_point(paths.session_directory(id));
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse) return lighter::outcome_error(Error::storage("session directory is a symlink, junction, or reparse point"));
    const auto database_exists = std::filesystem::exists(paths.session_database(id), error);
    if (error) return lighter::outcome_error(Error::storage("cannot inspect session database: " + error.message()));
    if (!database_exists) return lighter::outcome_error(Error::storage("published session database is absent", ErrorCode::NOT_FOUND));
    if (!std::filesystem::is_regular_file(paths.session_database(id), error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect session database: " + error.message()));
        return lighter::outcome_error(Error::storage("published session database is not a regular file"));
    }
    reparse = detail::is_reparse_point(paths.session_database(id));
    if (!reparse) return lighter::outcome_error(std::move(reparse).error());
    if (*reparse) return lighter::outcome_error(Error::storage("session database is a symlink, junction, or reparse point"));
    auto writer_state = std::make_shared<SessionWriter::State>(state, *std::move(lease), id);
    auto opened = open_database(paths.session_database(id), false, true);
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    writer_state->database = *opened;
    writer_state->published = true;
    return SessionWriter(std::move(writer_state));
}

Result<bool> SessionRepository::remove_catalog_hint_if_authority_absent(SessionId id) const {
    const StatePaths paths{state->root};
    auto lease = acquire_session_lease(state->root, id);
    if (!lease) return lighter::outcome_error(std::move(lease).error());
    std::error_code error;
    const auto authority_exists = std::filesystem::exists(paths.session_database(id), error);
    if (error) return lighter::outcome_error(Error::storage("cannot recheck session authority: " + error.message()));
    if (authority_exists) return false;
    if (auto removed = state->catalog.remove(id); !removed) return lighter::outcome_error(std::move(removed).error());
    return true;
}

Result<SessionId> SessionRepository::resolve_exact(std::string_view value) const {
    if (value.size() != 36) return lighter::outcome_error(Error::config("a full session ID must contain 36 characters"));
    auto id = parse_session_id(value);
    if (!id) return lighter::outcome_error(std::move(id).error());
    const StatePaths paths{state->root};
    std::error_code error;
    if (std::filesystem::exists(paths.session_database(*id), error)) {
        auto reparse = detail::is_reparse_point(paths.session_database(*id));
        if (!reparse) return lighter::outcome_error(std::move(reparse).error());
        if (*reparse) return lighter::outcome_error(Error::storage("session database is a symlink, junction, or reparse point"));
    }
    if (error) return lighter::outcome_error(Error::storage("cannot inspect exact session path: " + error.message()));
    if (!std::filesystem::is_regular_file(paths.session_database(*id), error)) {
        if (error) return lighter::outcome_error(Error::storage("cannot inspect exact session path: " + error.message()));
        return lighter::outcome_error(Error::storage("session was not found"));
    }
    return *id;
}

Result<CatalogReconciliation> SessionRepository::reconcile_pending() const {
    const StatePaths paths{state->root};
    CatalogReconciliation result;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(paths.catalog_pending(), iteration_error), end; !iteration_error && iterator != end;
         iterator.increment(iteration_error)) {
        auto id = canonical_path_id(iterator->path());
        if (!id) {
            result.warnings.push_back(id.error().message());
            continue;
        }
        auto reparse = detail::is_reparse_point(iterator->path());
        if (!reparse || *reparse) {
            result.warnings.push_back("catalog marker is unreadable or a reparse point: " + iterator->path().filename().string());
            continue;
        }
        auto target = read_marker(iterator->path());
        if (!target) {
            result.warnings.push_back("catalog marker " + to_string(*id) + ": " + target.error().message());
            continue;
        }
        auto lease = acquire_session_lease(state->root, *id);
        if (!lease) {
            if (lease.error().detail.contains("in use by another"))
                ++result.busy;
            else
                result.warnings.push_back("cannot lease pending session " + to_string(*id) + ": " + lease.error().message());
            continue;
        }
        std::error_code absent_error;
        const auto absent = !std::filesystem::exists(paths.session_database(*id), absent_error);
        if (absent_error) {
            result.warnings.push_back("cannot inspect pending session " + to_string(*id) + ": " + absent_error.message());
            continue;
        }
        if (absent) {
            if (auto removed_row = state->catalog.remove(*id); !removed_row) {
                result.warnings.push_back(removed_row.error().message());
                continue;
            }
            if (auto removed = detail::durable_remove_file(iterator->path()); !removed)
                result.warnings.push_back(removed.error().message());
            else
                ++result.repaired;
            continue;
        }
        auto projection = read_published_projection(paths, *id);
        if (!projection) {
            result.warnings.push_back("cannot read authoritative singleton for " + to_string(*id) + ": " + projection.error().message());
            continue;
        }
        if (auto projected = state->catalog.upsert(*projection); !projected) {
            result.warnings.push_back("cannot refresh catalog for " + to_string(*id) + ": " + projected.error().message());
            continue;
        }
        auto current = read_marker(iterator->path());
        if (!current) {
            result.warnings.push_back(current.error().message());
            continue;
        }
        if (*current == *target) {
            if (auto removed = detail::durable_remove_file(iterator->path()); !removed)
                result.warnings.push_back(removed.error().message());
            else
                ++result.repaired;
        }
    }
    if (iteration_error)
        return lighter::outcome_error(Error::storage("cannot enumerate catalog-pending directory: " + iteration_error.message()));
    return result;
}

Result<CatalogReconciliation> SessionRepository::rebuild_catalog() const {
    const StatePaths paths{state->root};
    CatalogReconciliation result;
    std::vector<SessionId> authoritative;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(paths.sessions(), iteration_error), end; !iteration_error && iterator != end;
         iterator.increment(iteration_error)) {
        auto id = canonical_path_id(iterator->path());
        if (!id) {
            result.warnings.push_back("invalid published session directory: " + iterator->path().filename().string());
            continue;
        }
        auto projection = read_published_projection(paths, *id);
        if (!projection) {
            result.warnings.push_back("cannot read published session singleton " + to_string(*id) + ": " + projection.error().message());
            continue;
        }
        if (auto projected = state->catalog.upsert(*projection); !projected) return lighter::outcome_error(std::move(projected).error());
        authoritative.push_back(*id);
        ++result.repaired;
    }
    if (iteration_error) return lighter::outcome_error(Error::storage("cannot enumerate published sessions: " + iteration_error.message()));
    auto catalog_ids = state->catalog.ids();
    if (!catalog_ids) return lighter::outcome_error(std::move(catalog_ids).error());
    for (const auto id : *catalog_ids) {
        if (std::ranges::find(authoritative, id) != authoritative.end()) continue;
        auto lease = acquire_session_lease(state->root, id);
        if (!lease) continue;
        std::error_code error;
        if (!std::filesystem::exists(paths.session_database(id), error) && !error) {
            if (auto removed = state->catalog.remove(id); !removed) result.warnings.push_back(removed.error().message());
        }
    }
    return result;
}

} // namespace liminal::session
