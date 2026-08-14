#include "store.h"

#include "codec.h"
#include "lease.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <utility>

#include <sqlite3.h>

#include <lighter/async/vocab/outcome.h>
#include <lighter/encoding/utf8.h>

namespace liminal::session {

namespace {

constexpr int k_application_id = 0x4c494d4e;
constexpr int k_schema_version = 1;

std::string path_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

Error sqlite_error(sqlite3 *database, std::string_view action, int code = SQLITE_ERROR) {
    auto detail = std::string(action) + ": " + (database ? sqlite3_errmsg(database) : sqlite3_errstr(code));
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session store busy: " + detail;
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
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) detail = "session store busy: " + detail;
    return lighter::outcome_error(Error::storage(std::move(detail)));
}

void bind_id(sqlite3_stmt *statement, int index, SessionId id) {
    sqlite3_bind_blob(statement, index, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
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

void bind_optional_text(sqlite3_stmt *statement, int index, const std::optional<std::string> &value) {
    if (value) {
        sqlite3_bind_text(statement, index, value->data(), static_cast<int>(value->size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(statement, index);
    }
}

void bind_optional_i64(sqlite3_stmt *statement, int index, const std::optional<i64> &value) {
    if (value)
        sqlite3_bind_int64(statement, index, *value);
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
        return lighter::outcome_error(Error::storage("session catalog contains an invalid UUID"));
    }
    SessionId id;
    std::memcpy(id.bytes.data(), sqlite3_column_blob(statement, column), id.bytes.size());
    return id;
}

Result<bool> has_user_schema_objects(sqlite3 *database) {
    auto query = prepare(database, R"sql(
SELECT EXISTS(
    SELECT 1 FROM sqlite_schema
    WHERE type IN ('table', 'index', 'view', 'trigger') AND name NOT LIKE 'sqlite\_%' ESCAPE '\'
)
)sql");
    if (!query) return lighter::outcome_error(std::move(query).error());
    if (sqlite3_step(query->value) != SQLITE_ROW) {
        return lighter::outcome_error(sqlite_error(database, "cannot inspect state database schema"));
    }
    return sqlite3_column_int(query->value, 0) != 0;
}

Result<void> migrate(sqlite3 *database) {
    auto application = prepare(database, "PRAGMA application_id");
    if (!application) return lighter::outcome_error(std::move(application).error());
    if (sqlite3_step(application->value) != SQLITE_ROW) return lighter::outcome_error(sqlite_error(database, "cannot read application id"));
    const auto application_id = sqlite3_column_int(application->value, 0);
    if (application_id != 0 && application_id != k_application_id) {
        return lighter::outcome_error(Error::storage("state database belongs to another application"));
    }
    sqlite3_finalize(application->value);
    application->value = nullptr;
    auto version = prepare(database, "PRAGMA user_version");
    if (!version) return lighter::outcome_error(std::move(version).error());
    if (sqlite3_step(version->value) != SQLITE_ROW) return lighter::outcome_error(sqlite_error(database, "cannot read schema version"));
    const auto schema_version = sqlite3_column_int(version->value, 0);
    sqlite3_finalize(version->value);
    version->value = nullptr;
    if (schema_version > k_schema_version) {
        return lighter::outcome_error(
            Error::storage("state database schema version " + std::to_string(schema_version) + " is newer than this Liminal build"));
    }
    if (schema_version != 0 && application_id != k_application_id) {
        return lighter::outcome_error(Error::storage("state database does not have Liminal's application identity"));
    }
    if (schema_version == k_schema_version) return {};
    if (schema_version != 0) return lighter::outcome_error(Error::storage("unsupported state database schema version"));
    if (application_id == 0) {
        auto has_objects = has_user_schema_objects(database);
        if (!has_objects) return lighter::outcome_error(std::move(has_objects).error());
        if (*has_objects) {
            return lighter::outcome_error(Error::storage("unidentified non-empty SQLite database cannot be used as Liminal state"));
        }
    }
    constexpr std::string_view migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE sessions (
    id BLOB PRIMARY KEY CHECK(length(id) = 16),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    workspace_root TEXT,
    workspace_key TEXT,
    working_directory TEXT NOT NULL,
    title TEXT,
    preview TEXT NOT NULL DEFAULT '',
    active_leaf_entry_id INTEGER,
    next_entry_id INTEGER NOT NULL DEFAULT 1,
    next_task_id INTEGER NOT NULL DEFAULT 1,
    next_provider_call_id INTEGER NOT NULL DEFAULT 1,
    entry_count INTEGER NOT NULL DEFAULT 0,
    tokens_used INTEGER NOT NULL DEFAULT 0,
    last_provider TEXT,
    last_model TEXT,
    last_reasoning_effort TEXT,
    archived_at_ms INTEGER,
    revision INTEGER NOT NULL DEFAULT 0,
    forked_from_session_id BLOB,
    forked_from_entry_id INTEGER
);
CREATE TABLE session_entries (
    session_id BLOB NOT NULL,
    entry_id INTEGER NOT NULL,
    task_id INTEGER,
    provider_call_id INTEGER,
    parent_entry_id INTEGER,
    kind INTEGER NOT NULL,
    payload_version INTEGER NOT NULL,
    payload_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    PRIMARY KEY (session_id, entry_id),
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, parent_entry_id) REFERENCES session_entries(session_id, entry_id)
);
CREATE INDEX sessions_workspace_recent ON sessions(workspace_key, archived_at_ms, updated_at_ms DESC, id DESC);
CREATE INDEX sessions_recent ON sessions(archived_at_ms, updated_at_ms DESC, id DESC);
CREATE INDEX session_entries_children ON session_entries(session_id, parent_entry_id);
CREATE INDEX session_entries_provider_calls ON session_entries(session_id, task_id, provider_call_id, entry_id);
PRAGMA application_id = 1279872334;
PRAGMA user_version = 1;
COMMIT;
)sql";
    return execute(database, migration);
}

Result<void> configure(sqlite3 *database) {
    if (sqlite3_libversion_number() < 3051003 || sqlite3_libversion_number() >= 4000000) {
        return lighter::outcome_error(Error::storage("SQLite 3.51.3 or newer (and older than 4.0) is required"));
    }
    if (auto result = execute(database, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA busy_timeout=5000;"); !result)
        return result;
    if (auto migrated = migrate(database); !migrated) return migrated;
    auto wal = prepare(database, "PRAGMA journal_mode=WAL");
    if (!wal) return lighter::outcome_error(std::move(wal).error());
    if (sqlite3_step(wal->value) != SQLITE_ROW || text(wal->value, 0) != "wal") {
        return lighter::outcome_error(Error::storage("state database did not enter WAL journal mode"));
    }
    sqlite3_finalize(wal->value);
    wal->value = nullptr;
    return {};
}

Result<void> begin(sqlite3 *database) { return execute(database, "BEGIN IMMEDIATE"); }

struct Rollback {
    sqlite3 *database;
    bool active = true;
    ~Rollback() {
        if (active) sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

} // namespace

struct Store::State {
    ~State() {
        if (database) sqlite3_close(database);
    }

    std::filesystem::path path;
    sqlite3 *database = nullptr;
    mutable std::mutex mutex;
};

struct SessionWriter::State {
    State(std::shared_ptr<Store::State> store, SessionLease lease, SessionId id)
        : store(std::move(store)), lease(std::move(lease)), id(id) {}

    std::shared_ptr<Store::State> store;
    SessionLease lease;
    SessionId id;
    u64 revision = 0;
};

SessionWriter::~SessionWriter() = default;
SessionWriter::SessionWriter(SessionWriter &&) noexcept = default;
SessionWriter &SessionWriter::operator=(SessionWriter &&) noexcept = default;

SessionId SessionWriter::session_id() const noexcept { return state->id; }

Result<Store> Store::open(std::filesystem::path database_path) {
    std::error_code directory_error;
    const auto directory = database_path.parent_path();
    bool created_directory = false;
    if (!directory.empty()) {
        created_directory = std::filesystem::create_directories(directory, directory_error);
        if (directory_error) {
            return lighter::outcome_error(Error::storage("cannot create state directory: " + directory_error.message()));
        }
#ifndef _WIN32
        if (created_directory) {
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                         directory_error);
            if (directory_error)
                return lighter::outcome_error(Error::storage("cannot secure state directory: " + directory_error.message()));
        }
#endif
    }
    std::error_code database_error;
    const auto database_existed = std::filesystem::exists(database_path, database_error);
    if (database_error) return lighter::outcome_error(Error::storage("cannot inspect state database: " + database_error.message()));
    auto state = std::make_shared<State>();
    state->path = std::move(database_path);
    const auto encoded_path = path_utf8(state->path);
    const auto code = sqlite3_open_v2(encoded_path.c_str(), &state->database,
                                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) return lighter::outcome_error(sqlite_error(state->database, "cannot open state database", code));
#ifndef _WIN32
    if (!database_existed) {
        std::filesystem::permissions(state->path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, directory_error);
        if (directory_error) return lighter::outcome_error(Error::storage("cannot secure state database: " + directory_error.message()));
    }
#endif
    if (auto configured = configure(state->database); !configured) return lighter::outcome_error(std::move(configured).error());
    return Store(std::move(state));
}

const std::filesystem::path &Store::path() const noexcept { return state->path; }

Result<SessionWriter> Store::lease(SessionId id) const {
    auto lease = acquire_session_lease(state->path, id);
    if (!lease) return lighter::outcome_error(std::move(lease).error());
    return SessionWriter(std::make_shared<SessionWriter::State>(state, *std::move(lease), id));
}

Result<Session> SessionWriter::load() {
    const auto &store = state->store;
    const auto id = state->id;
    std::scoped_lock lock(store->mutex);
    auto catalog = prepare(store->database, R"sql(
SELECT created_at_ms, updated_at_ms, workspace_root, workspace_key, working_directory, title, preview,
       active_leaf_entry_id, next_entry_id, next_task_id, next_provider_call_id, last_provider, last_model,
       last_reasoning_effort, archived_at_ms, revision, forked_from_session_id, forked_from_entry_id
FROM sessions WHERE id = ?1
)sql");
    if (!catalog) return lighter::outcome_error(std::move(catalog).error());
    bind_id(catalog->value, 1, id);
    const auto row = sqlite3_step(catalog->value);
    if (row == SQLITE_DONE) return lighter::outcome_error(Error::storage("session was not found"));
    if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(store->database, "cannot load session catalog", row));

    Session session(id);
    session.metadata.created_at_ms = sqlite3_column_int64(catalog->value, 0);
    session.metadata.updated_at_ms = sqlite3_column_int64(catalog->value, 1);
    auto workspace_root = optional_text(catalog->value, 2);
    auto workspace_key = optional_text(catalog->value, 3);
    if (workspace_root.has_value() != workspace_key.has_value()) {
        return lighter::outcome_error(Error::storage("durable session has incomplete workspace metadata"));
    }
    if (workspace_root) session.metadata.workspace = SessionWorkspace{*std::move(workspace_root), *std::move(workspace_key)};
    session.metadata.working_directory = text(catalog->value, 4);
    session.metadata.title = optional_text(catalog->value, 5);
    session.metadata.preview = text(catalog->value, 6);
    if (sqlite3_column_type(catalog->value, 7) != SQLITE_NULL)
        session.active_leaf = EntryId{static_cast<u64>(sqlite3_column_int64(catalog->value, 7))};
    session.next_entry_id = static_cast<u64>(sqlite3_column_int64(catalog->value, 8));
    session.next_task_id = static_cast<u64>(sqlite3_column_int64(catalog->value, 9));
    session.next_provider_call_id = static_cast<u64>(sqlite3_column_int64(catalog->value, 10));
    auto provider = optional_text(catalog->value, 11);
    auto model = optional_text(catalog->value, 12);
    auto reasoning_effort = optional_text(catalog->value, 13);
    if (provider.has_value() != model.has_value() || (reasoning_effort && !provider)) {
        return lighter::outcome_error(Error::storage("durable session has incomplete model preference"));
    }
    if (provider) {
        session.metadata.model_preference = SessionModelPreference{
            .provider = *std::move(provider),
            .model = *std::move(model),
            .reasoning_effort = std::move(reasoning_effort),
        };
    }
    if (sqlite3_column_type(catalog->value, 14) != SQLITE_NULL) session.metadata.archived_at_ms = sqlite3_column_int64(catalog->value, 14);
    const auto revision = static_cast<u64>(sqlite3_column_int64(catalog->value, 15));
    std::optional<SessionId> forked_session;
    if (sqlite3_column_type(catalog->value, 16) != SQLITE_NULL) {
        auto source = column_id(catalog->value, 16);
        if (!source) return lighter::outcome_error(std::move(source).error());
        forked_session = *source;
    }
    std::optional<EntryId> forked_entry;
    if (sqlite3_column_type(catalog->value, 17) != SQLITE_NULL) {
        forked_entry = EntryId{static_cast<u64>(sqlite3_column_int64(catalog->value, 17))};
    }
    if (forked_session.has_value() != forked_entry.has_value()) {
        return lighter::outcome_error(Error::storage("durable session has incomplete fork origin"));
    }
    if (forked_session) session.metadata.forked_from = ForkOrigin{*forked_session, *forked_entry};

    auto entries = prepare(store->database, R"sql(
SELECT entry_id, task_id, provider_call_id, parent_entry_id, kind, payload_version, payload_json, created_at_ms
FROM session_entries WHERE session_id = ?1 ORDER BY entry_id
)sql");
    if (!entries) return lighter::outcome_error(std::move(entries).error());
    bind_id(entries->value, 1, id);
    while (true) {
        const auto entry_row = sqlite3_step(entries->value);
        if (entry_row == SQLITE_DONE) break;
        if (entry_row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(store->database, "cannot load session entries", entry_row));
        const auto kind = static_cast<EntryKind>(sqlite3_column_int(entries->value, 4));
        const auto version = static_cast<u32>(sqlite3_column_int(entries->value, 5));
        auto payload = decode_payload(kind, version, text(entries->value, 6));
        if (!payload) return lighter::outcome_error(std::move(payload).error());
        auto encoded = encode_payload(*payload);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        const auto task_column = sqlite3_column_type(entries->value, 1) == SQLITE_NULL ?
                                     std::optional<u64>{} :
                                     static_cast<u64>(sqlite3_column_int64(entries->value, 1));
        const auto call_column = sqlite3_column_type(entries->value, 2) == SQLITE_NULL ?
                                     std::optional<u64>{} :
                                     static_cast<u64>(sqlite3_column_int64(entries->value, 2));
        if (task_column != (encoded->task_id ? std::optional<u64>{encoded->task_id->value} : std::nullopt) ||
            call_column != (encoded->provider_call_id ? std::optional<u64>{encoded->provider_call_id->value} : std::nullopt)) {
            return lighter::outcome_error(Error::storage("session entry lifecycle index does not match its payload"));
        }
        SessionEntry entry{
            .id = {static_cast<u64>(sqlite3_column_int64(entries->value, 0))},
            .payload = *std::move(payload),
            .created_at_ms = sqlite3_column_int64(entries->value, 7),
        };
        if (sqlite3_column_type(entries->value, 3) != SQLITE_NULL) {
            entry.parent_id = EntryId{static_cast<u64>(sqlite3_column_int64(entries->value, 3))};
        }
        session.entries.push_back(std::move(entry));
    }
    auto valid = session.validate();
    if (!valid) return lighter::outcome_error(Error::storage("invalid durable session: " + valid.error().detail));
    state->revision = revision;
    return session;
}

Result<SessionId> Store::resolve_id(std::string_view value) const {
    if (value.size() == 36) {
        auto parsed = parse_session_id(value);
        if (!parsed) return lighter::outcome_error(std::move(parsed).error());
        std::scoped_lock lock(state->mutex);
        auto found = prepare(state->database, "SELECT 1 FROM sessions WHERE id=?1");
        if (!found) return lighter::outcome_error(std::move(found).error());
        bind_id(found->value, 1, *parsed);
        const auto row = sqlite3_step(found->value);
        if (row == SQLITE_ROW) return *parsed;
        if (row == SQLITE_DONE) return lighter::outcome_error(Error::storage("session was not found"));
        return lighter::outcome_error(sqlite_error(state->database, "cannot resolve session id", row));
    }
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
    auto matches = upper ? prepare(state->database, "SELECT id FROM sessions WHERE id >= ?1 AND id < ?2 ORDER BY id LIMIT 2") :
                           prepare(state->database, "SELECT id FROM sessions WHERE id >= ?1 ORDER BY id LIMIT 2");
    if (!matches) return lighter::outcome_error(std::move(matches).error());
    bind_id(matches->value, 1, lower);
    if (upper) bind_id(matches->value, 2, *upper);
    std::optional<SessionId> result;
    while (true) {
        const auto row = sqlite3_step(matches->value);
        if (row == SQLITE_DONE) break;
        if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot resolve session id prefix", row));
        auto id = column_id(matches->value, 0);
        if (!id) return lighter::outcome_error(std::move(id).error());
        if (result) return lighter::outcome_error(Error::storage("session id prefix is ambiguous"));
        result = *id;
    }
    if (!result) return lighter::outcome_error(Error::storage("session was not found"));
    return *result;
}

Result<SessionSummary> Store::latest(std::string_view workspace_key) const {
    std::scoped_lock lock(state->mutex);
    auto query = prepare(state->database, R"sql(
SELECT id, created_at_ms, updated_at_ms, workspace_root, title, preview, last_provider, last_model,
       last_reasoning_effort, entry_count, tokens_used
FROM sessions
WHERE workspace_key=?1 AND archived_at_ms IS NULL
ORDER BY updated_at_ms DESC, id DESC LIMIT 1
)sql");
    if (!query) return lighter::outcome_error(std::move(query).error());
    sqlite3_bind_text(query->value, 1, workspace_key.data(), static_cast<int>(workspace_key.size()), SQLITE_TRANSIENT);
    const auto row = sqlite3_step(query->value);
    if (row == SQLITE_DONE) return lighter::outcome_error(Error::storage("no saved session exists in this workspace"));
    if (row != SQLITE_ROW) return lighter::outcome_error(sqlite_error(state->database, "cannot find latest session", row));
    auto id = column_id(query->value, 0);
    if (!id) return lighter::outcome_error(std::move(id).error());
    auto provider = optional_text(query->value, 6);
    auto model = optional_text(query->value, 7);
    auto reasoning_effort = optional_text(query->value, 8);
    if (provider.has_value() != model.has_value() || (reasoning_effort && !provider)) {
        return lighter::outcome_error(Error::storage("session catalog has an incomplete model preference"));
    }
    std::optional<SessionModelPreference> model_preference;
    if (provider) {
        model_preference = SessionModelPreference{
            .provider = *std::move(provider),
            .model = *std::move(model),
            .reasoning_effort = std::move(reasoning_effort),
        };
    }
    return SessionSummary{
        .id = *id,
        .created_at_ms = sqlite3_column_int64(query->value, 1),
        .updated_at_ms = sqlite3_column_int64(query->value, 2),
        .workspace_root = optional_text(query->value, 3),
        .title = optional_text(query->value, 4),
        .preview = text(query->value, 5),
        .model_preference = std::move(model_preference),
        .entry_count = static_cast<u64>(sqlite3_column_int64(query->value, 9)),
        .tokens_used = static_cast<u64>(sqlite3_column_int64(query->value, 10)),
    };
}

namespace {

struct DurableHead {
    bool exists = false;
    u64 entry_count = 0;
    u64 next_task_id = 1;
    u64 next_provider_call_id = 1;
    u64 tokens_used = 0;
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
};

Result<void> validate_delta(const SessionDelta &delta, const DurableHead &head) {
    if (!head.exists && delta.entries.empty()) {
        return lighter::outcome_error(Error::storage("cannot materialize a session without a semantic entry"));
    }
    if (delta.metadata.created_at_ms <= 0 || delta.metadata.updated_at_ms < delta.metadata.created_at_ms ||
        (head.exists && (delta.metadata.created_at_ms != head.created_at_ms || delta.metadata.updated_at_ms < head.updated_at_ms))) {
        return lighter::outcome_error(Error::storage("session delta has invalid catalog timestamps"));
    }
    if (delta.metadata.preview.size() > 240 || !lighter::encoding::utf8::is_valid(delta.metadata.preview)) {
        return lighter::outcome_error(Error::storage("session delta has an invalid preview"));
    }
    if (delta.metadata.workspace && (delta.metadata.workspace->root.empty() || delta.metadata.workspace->key.empty())) {
        return lighter::outcome_error(Error::storage("session delta has incomplete workspace metadata"));
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
            [&maximum_task_id, &maximum_provider_call_id](const auto &payload) {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<T, TaskStarted> || std::same_as<T, TaskFinished>) {
                    maximum_task_id = std::max(maximum_task_id, payload.id.value);
                } else if constexpr (std::same_as<T, OutputItemCompleted> || std::same_as<T, ToolResults>) {
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

} // namespace

Result<void> SessionWriter::commit(const SessionDelta &delta) {
    const auto &store = state->store;
    const auto id = state->id;
    std::scoped_lock lock(store->mutex);
    auto started = begin(store->database);
    if (!started) return lighter::outcome_error(std::move(started).error());
    Rollback rollback{store->database};

    auto revision = prepare(store->database,
                            "SELECT revision, entry_count, next_task_id, next_provider_call_id, tokens_used, created_at_ms, updated_at_ms "
                            "FROM sessions WHERE id=?1");
    if (!revision) return lighter::outcome_error(std::move(revision).error());
    bind_id(revision->value, 1, id);
    auto row = sqlite3_step(revision->value);
    DurableHead head;
    if (row == SQLITE_DONE) {
        if (state->revision != 0) return lighter::outcome_error(Error::storage("session revision conflict: missing session"));
    } else if (row != SQLITE_ROW) {
        return lighter::outcome_error(sqlite_error(store->database, "cannot inspect session revision", row));
    } else {
        if (static_cast<u64>(sqlite3_column_int64(revision->value, 0)) != state->revision) {
            return lighter::outcome_error(Error::storage("session revision conflict"));
        }
        head = {
            .exists = true,
            .entry_count = static_cast<u64>(sqlite3_column_int64(revision->value, 1)),
            .next_task_id = static_cast<u64>(sqlite3_column_int64(revision->value, 2)),
            .next_provider_call_id = static_cast<u64>(sqlite3_column_int64(revision->value, 3)),
            .tokens_used = static_cast<u64>(sqlite3_column_int64(revision->value, 4)),
            .created_at_ms = sqlite3_column_int64(revision->value, 5),
            .updated_at_ms = sqlite3_column_int64(revision->value, 6),
        };
    }
    if (auto valid = validate_delta(delta, head); !valid) return valid;

    if (!head.exists) {
        auto insert_session = prepare(store->database, R"sql(
INSERT INTO sessions(id, created_at_ms, updated_at_ms, workspace_root, workspace_key, working_directory, title, preview,
                     active_leaf_entry_id, next_entry_id, next_task_id, next_provider_call_id, entry_count, tokens_used,
                     last_provider, last_model, last_reasoning_effort, archived_at_ms, revision,
                     forked_from_session_id, forked_from_entry_id)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,NULL,1,1,1,0,0,?9,?10,?11,?12,0,?13,?14)
)sql");
        if (!insert_session) return lighter::outcome_error(std::move(insert_session).error());
        bind_id(insert_session->value, 1, id);
        sqlite3_bind_int64(insert_session->value, 2, delta.metadata.created_at_ms);
        sqlite3_bind_int64(insert_session->value, 3, delta.metadata.updated_at_ms);
        bind_optional_text(insert_session->value, 4,
                           delta.metadata.workspace ? std::optional{delta.metadata.workspace->root} : std::nullopt);
        bind_optional_text(insert_session->value, 5,
                           delta.metadata.workspace ? std::optional{delta.metadata.workspace->key} : std::nullopt);
        sqlite3_bind_text(insert_session->value, 6, delta.metadata.working_directory.data(),
                          static_cast<int>(delta.metadata.working_directory.size()), SQLITE_TRANSIENT);
        bind_optional_text(insert_session->value, 7, delta.metadata.title);
        sqlite3_bind_text(insert_session->value, 8, delta.metadata.preview.data(), static_cast<int>(delta.metadata.preview.size()),
                          SQLITE_TRANSIENT);
        bind_optional_text(insert_session->value, 9,
                           delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->provider} : std::nullopt);
        bind_optional_text(insert_session->value, 10,
                           delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->model} : std::nullopt);
        bind_optional_text(insert_session->value, 11,
                           delta.metadata.model_preference ? delta.metadata.model_preference->reasoning_effort : std::nullopt);
        bind_optional_i64(insert_session->value, 12, delta.metadata.archived_at_ms);
        bind_optional_session(insert_session->value, 13,
                              delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->session} : std::nullopt);
        bind_optional_entry(insert_session->value, 14,
                            delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->entry} : std::nullopt);
        if (sqlite3_step(insert_session->value) != SQLITE_DONE) {
            return lighter::outcome_error(sqlite_error(store->database, "cannot create session"));
        }
    }

    auto insert_entry = prepare(store->database, R"sql(
INSERT INTO session_entries(session_id,entry_id,task_id,provider_call_id,parent_entry_id,kind,payload_version,payload_json,created_at_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)
)sql");
    if (!insert_entry) return lighter::outcome_error(std::move(insert_entry).error());
    for (const auto &entry : delta.entries) {
        auto encoded = encode_payload(entry.payload);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        sqlite3_reset(insert_entry->value);
        sqlite3_clear_bindings(insert_entry->value);
        bind_id(insert_entry->value, 1, id);
        sqlite3_bind_int64(insert_entry->value, 2, static_cast<sqlite3_int64>(entry.id.value));
        if (encoded->task_id)
            sqlite3_bind_int64(insert_entry->value, 3, static_cast<sqlite3_int64>(encoded->task_id->value));
        else
            sqlite3_bind_null(insert_entry->value, 3);
        if (encoded->provider_call_id)
            sqlite3_bind_int64(insert_entry->value, 4, static_cast<sqlite3_int64>(encoded->provider_call_id->value));
        else
            sqlite3_bind_null(insert_entry->value, 4);
        bind_optional_entry(insert_entry->value, 5, entry.parent_id);
        sqlite3_bind_int(insert_entry->value, 6, static_cast<int>(encoded->kind));
        sqlite3_bind_int(insert_entry->value, 7, static_cast<int>(encoded->version));
        sqlite3_bind_text(insert_entry->value, 8, encoded->json.data(), static_cast<int>(encoded->json.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_entry->value, 9, entry.created_at_ms);
        const auto inserted = sqlite3_step(insert_entry->value);
        if (inserted != SQLITE_DONE) return lighter::outcome_error(sqlite_error(store->database, "cannot append session entry", inserted));
    }

    auto update = prepare(store->database, R"sql(
UPDATE sessions SET updated_at_ms=?2, workspace_root=?3, workspace_key=?4, working_directory=?5, title=?6, preview=?7,
 active_leaf_entry_id=?8, next_entry_id=?9, next_task_id=?10, next_provider_call_id=?11, entry_count=?12,
 tokens_used=?13, last_provider=?14, last_model=?15, last_reasoning_effort=?16, archived_at_ms=?17,
 revision=revision+1, forked_from_session_id=?18, forked_from_entry_id=?19
WHERE id=?1 AND revision=?20
)sql");
    if (!update) return lighter::outcome_error(std::move(update).error());
    bind_id(update->value, 1, id);
    sqlite3_bind_int64(update->value, 2, delta.metadata.updated_at_ms);
    bind_optional_text(update->value, 3, delta.metadata.workspace ? std::optional{delta.metadata.workspace->root} : std::nullopt);
    bind_optional_text(update->value, 4, delta.metadata.workspace ? std::optional{delta.metadata.workspace->key} : std::nullopt);
    sqlite3_bind_text(update->value, 5, delta.metadata.working_directory.data(), static_cast<int>(delta.metadata.working_directory.size()),
                      SQLITE_TRANSIENT);
    bind_optional_text(update->value, 6, delta.metadata.title);
    sqlite3_bind_text(update->value, 7, delta.metadata.preview.data(), static_cast<int>(delta.metadata.preview.size()), SQLITE_TRANSIENT);
    bind_optional_entry(update->value, 8, delta.active_leaf);
    sqlite3_bind_int64(update->value, 9, static_cast<sqlite3_int64>(delta.next_entry_id));
    sqlite3_bind_int64(update->value, 10, static_cast<sqlite3_int64>(delta.next_task_id));
    sqlite3_bind_int64(update->value, 11, static_cast<sqlite3_int64>(delta.next_provider_call_id));
    sqlite3_bind_int64(update->value, 12, static_cast<sqlite3_int64>(delta.entry_count));
    sqlite3_bind_int64(update->value, 13, static_cast<sqlite3_int64>(delta.tokens_used));
    bind_optional_text(update->value, 14,
                       delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->provider} : std::nullopt);
    bind_optional_text(update->value, 15,
                       delta.metadata.model_preference ? std::optional{delta.metadata.model_preference->model} : std::nullopt);
    bind_optional_text(update->value, 16,
                       delta.metadata.model_preference ? delta.metadata.model_preference->reasoning_effort : std::nullopt);
    bind_optional_i64(update->value, 17, delta.metadata.archived_at_ms);
    bind_optional_session(update->value, 18,
                          delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->session} : std::nullopt);
    bind_optional_entry(update->value, 19, delta.metadata.forked_from ? std::optional{delta.metadata.forked_from->entry} : std::nullopt);
    sqlite3_bind_int64(update->value, 20, static_cast<sqlite3_int64>(state->revision));
    const auto updated = sqlite3_step(update->value);
    if (updated != SQLITE_DONE) return lighter::outcome_error(sqlite_error(store->database, "cannot update session catalog", updated));
    if (sqlite3_changes(store->database) != 1) return lighter::outcome_error(Error::storage("session revision conflict"));
    auto committed = execute(store->database, "COMMIT");
    if (!committed) return lighter::outcome_error(std::move(committed).error());
    rollback.active = false;
    ++state->revision;
    return {};
}

SessionDelta make_delta(const Session &session, std::span<const SessionEntry> entries) {
    return {
        .entries = {entries.begin(), entries.end()},
        .active_leaf = session.active_leaf,
        .next_entry_id = session.next_entry_id,
        .next_task_id = session.next_task_id,
        .next_provider_call_id = session.next_provider_call_id,
        .entry_count = session.entries.size(),
        .tokens_used = session.tokens_used(),
        .metadata = session.metadata,
    };
}

} // namespace liminal::session
