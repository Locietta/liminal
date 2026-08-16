#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <sqlite3.h>

#include <glaze/json.hpp>

#include <liminal/application/session_coordinator.h>
#include <liminal/session/catalog.h>
#include <liminal/session/lease.h>
#include <liminal/session/paths.h>
#include <liminal/session/persistence.h>
#include <liminal/session/recovery.h>
#include <liminal/session/repository.h>
#include <liminal/session/store.h>
#include <liminal/session/store_test.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/hydration.h>

namespace {

using namespace liminal;
using namespace lighter::types;

[[noreturn]] void fail(std::string_view message) {
    std::fprintf(stderr, "test failure: %.*s\n", static_cast<int>(message.size()), message.data());
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

struct TemporaryState {
    TemporaryState() {
        root = std::filesystem::temp_directory_path() / ("liminal-session-test-" + session::to_string(session::generate_session_id()));
        std::error_code error;
        require(std::filesystem::create_directories(root, error) && !error, "failed to create temporary state root");
    }
    ~TemporaryState() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

struct StorageHookReset {
    explicit StorageHookReset(session::SessionRepository &repository) : repository(repository) {}
    ~StorageHookReset() { session::testing::set_storage_hook(repository, {}); }
    session::SessionRepository &repository;
};

struct Storage {
    session::SessionCatalog catalog;
    session::SessionRepository repository;
};

Result<Storage> open_storage(const std::filesystem::path &root) {
    auto repository = session::SessionRepository::open(root);
    if (!repository) return lighter::outcome_error(std::move(repository).error());
    auto catalog = repository->catalog();
    if (!catalog) return lighter::outcome_error(std::move(catalog).error());
    return Storage{.catalog = *std::move(catalog), .repository = *std::move(repository)};
}

session::Session make_session(std::string task, i64 admission_time = 1'000'000) {
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "C:/workspace", .key = "workspace"};
    value.metadata.working_directory = "C:/workspace";
    value.start_task(std::move(task), std::max(admission_time, value.metadata.created_at_ms));
    return value;
}

session::ConversationCheckpointId checkpoint_id(session::EntryId entry) { return {entry}; }

session::SessionId deterministic_id(u64 value) {
    session::SessionId id;
    for (usize index = 0; index < sizeof(value); ++index) {
        id.bytes[id.bytes.size() - 1 - index] = static_cast<u8>(value >> (index * 8));
    }
    return id;
}

provider::ToolCall tool_call(std::string id) {
    glz::generic input;
    require(!glz::read_json(input, R"({"path":"README.md"})"), "failed to create tool input fixture");
    return {.id = std::move(id), .name = "read_file", .input = std::move(input)};
}

struct Published {
    session::Session value;
    session::SessionWriter writer;
};

Result<Published> publish(session::SessionRepository repository, session::Session value) {
    auto writer = repository.create(value.id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto committed = writer->commit(session::make_delta(value, value.entries));
    if (!committed) return lighter::outcome_error(std::move(committed).error());
    return Published{.value = std::move(value), .writer = *std::move(writer)};
}

sqlite3 *open_sqlite(const std::filesystem::path &path) {
    sqlite3 *database = nullptr;
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK, "failed to open SQLite test connection");
    return database;
}

void execute(sqlite3 *database, const char *sql, std::string_view message) {
    char *detail = nullptr;
    const auto code = sqlite3_exec(database, sql, nullptr, nullptr, &detail);
    if (code != SQLITE_OK) {
        const auto description = detail ? std::string(detail) : sqlite3_errmsg(database);
        sqlite3_free(detail);
        fail(std::string(message) + ": " + description);
    }
}

std::string scalar_text(const std::filesystem::path &path, const char *sql) {
    auto *database = open_sqlite(path);
    sqlite3_stmt *statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK, "failed to prepare scalar query");
    require(sqlite3_step(statement) == SQLITE_ROW, "scalar query returned no row");
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    const auto result = value ? std::string(value) : std::string{};
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

i64 scalar_i64(const std::filesystem::path &path, const char *sql) {
    auto *database = open_sqlite(path);
    sqlite3_stmt *statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK, "failed to prepare integer query");
    require(sqlite3_step(statement) == SQLITE_ROW, "integer query returned no row");
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

void insert_catalog_rows(const std::filesystem::path &path, u64 first, u64 count, std::string_view workspace, i64 first_timestamp,
                         i64 timestamp_step = 1) {
    auto *database = open_sqlite(path);
    execute(database, "BEGIN IMMEDIATE", "failed to begin catalog fixture");
    sqlite3_stmt *insert = nullptr;
    constexpr auto sql = R"sql(
INSERT INTO sessions(id,observed_revision,workspace_key,updated_at_ms,title,preview)
VALUES(?1,1,?2,?3,?4,?5)
)sql";
    require(sqlite3_prepare_v2(database, sql, -1, &insert, nullptr) == SQLITE_OK, "failed to prepare catalog fixture insert");
    for (u64 offset = 0; offset < count; ++offset) {
        const auto id = deterministic_id(first + offset);
        const auto updated = first_timestamp + static_cast<i64>(offset) * timestamp_step;
        const auto title = "Session " + std::to_string(first + offset);
        const auto preview = "Preview " + std::to_string(first + offset);
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_blob(insert, 1, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, workspace.data(), static_cast<int>(workspace.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert, 3, updated);
        sqlite3_bind_text(insert, 4, title.data(), static_cast<int>(title.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 5, preview.data(), static_cast<int>(preview.size()), SQLITE_TRANSIENT);
        require(sqlite3_step(insert) == SQLITE_DONE, "failed to insert catalog fixture row");
    }
    sqlite3_finalize(insert);
    execute(database, "COMMIT", "failed to commit catalog fixture");
    sqlite3_close(database);
}

void write_marker(const std::filesystem::path &root, session::SessionId id, u64 revision) {
    const session::StatePaths paths{root};
    std::ofstream marker(paths.pending_marker(id), std::ios::binary | std::ios::trunc);
    marker << "1 " << revision << '\n';
    marker.close();
    require(static_cast<bool>(marker), "failed to write marker fixture");
}

u64 read_marker(const std::filesystem::path &root, session::SessionId id) {
    std::ifstream marker(session::StatePaths{root}.pending_marker(id), std::ios::binary);
    std::string version;
    u64 revision = 0;
    marker >> version >> revision;
    require(static_cast<bool>(marker) && version == "1", "failed to read marker fixture");
    return revision;
}

bool directory_empty(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::directory_iterator(path, error) == std::filesystem::directory_iterator{} && !error;
}

void test_topology_publication_and_minimal_schemas() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open per-session storage");
    const session::StatePaths paths{temporary.root};
    require(std::filesystem::is_regular_file(paths.catalog()) && std::filesystem::is_directory(paths.sessions()) &&
                std::filesystem::is_directory(paths.staging()) && std::filesystem::is_directory(paths.catalog_pending()) &&
                std::filesystem::is_directory(paths.locks()),
            "state-root topology was not created");

    auto value = make_session("publish me");
    const auto id = value.id;
    auto writer = storage->repository.create(id);
    require(writer.has_value() && !std::filesystem::exists(paths.session_directory(id)),
            "leasing an unused session materialized a durable session");
    auto committed = writer->commit(session::make_delta(value, value.entries));
    if (!committed) fail("initial staged publication failed: " + committed.error().message());
    require(std::filesystem::is_regular_file(paths.session_database(id)) && directory_empty(paths.staging()),
            "publication did not move one complete staged directory");
    require(scalar_text(paths.session_database(id), "PRAGMA journal_mode") == "wal", "published session was not reopened in WAL mode");
    require(!std::filesystem::exists(paths.pending_marker(id)), "successful publication left its pending marker");

    auto *database = open_sqlite(paths.session_database(id));
    sqlite3_stmt *columns = nullptr;
    require(sqlite3_prepare_v2(database, "PRAGMA table_info(session_entries)", -1, &columns, nullptr) == SQLITE_OK,
            "failed to inspect entry schema");
    std::set<std::string> entry_columns;
    while (sqlite3_step(columns) == SQLITE_ROW) {
        entry_columns.emplace(reinterpret_cast<const char *>(sqlite3_column_text(columns, 1)));
    }
    sqlite3_finalize(columns);
    sqlite3_close(database);
    require(!entry_columns.contains("session_id"), "per-session entry rows still repeat the session ID");

    database = open_sqlite(paths.catalog());
    require(sqlite3_prepare_v2(database, "PRAGMA table_info(sessions)", -1, &columns, nullptr) == SQLITE_OK,
            "failed to inspect catalog schema");
    std::set<std::string> catalog_columns;
    while (sqlite3_step(columns) == SQLITE_ROW) catalog_columns.emplace(reinterpret_cast<const char *>(sqlite3_column_text(columns, 1)));
    sqlite3_finalize(columns);
    sqlite3_close(database);
    require(catalog_columns == std::set<std::string>{"id", "observed_revision", "workspace_key", "updated_at_ms", "title", "preview"},
            "global catalog contains fields outside the bounded discovery projection");
}

void test_independent_session_write_transactions() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open concurrency storage");
    auto first = publish(storage->repository, make_session("first"));
    auto second = publish(storage->repository, make_session("second"));
    require(first && second, "failed to publish concurrency fixtures");
    const session::StatePaths paths{temporary.root};
    auto *left = open_sqlite(paths.session_database(first->value.id));
    auto *right = open_sqlite(paths.session_database(second->value.id));
    execute(left, "BEGIN IMMEDIATE", "failed to hold session A writer transaction");
    execute(right, "BEGIN IMMEDIATE", "session A blocked session B's independent writer transaction");
    execute(right, "COMMIT", "failed to commit session B transaction");
    execute(left, "COMMIT", "failed to commit session A transaction");
    sqlite3_close(right);
    sqlite3_close(left);
}

int lease_child(const std::filesystem::path &root, std::string_view id_text) {
    auto storage = open_storage(root);
    if (!storage) return 2;
    auto id = session::parse_session_id(id_text);
    if (!id) return 3;
    auto writer = storage->repository.acquire(*id);
    if (!writer) return 4;
    std::cout.put('R');
    std::cout.flush();
    std::cin.get();
    return 0;
}

void test_cross_process_lease_exclusion(const std::filesystem::path &executable) {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to open lease storage");
        auto published = publish(storage->repository, make_session("lease"));
        require(published.has_value(), "failed to publish lease fixture");
        id = published->value.id;
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE child_stdin_read = nullptr;
    HANDLE parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    require(CreatePipe(&child_stdin_read, &parent_stdin_write, &security, 0) &&
                CreatePipe(&parent_stdout_read, &child_stdout_write, &security, 0),
            "failed to create lease child pipes");
    SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stdout_write;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + executable.wstring() + L"\" --lease-child \"" + temporary.root.wstring() + L"\" " +
                   std::filesystem::path(session::to_string(id)).wstring();
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process),
            "failed to start lease child process");
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    char ready = 0;
    DWORD read = 0;
    require(ReadFile(parent_stdout_read, &ready, 1, &read, nullptr) && read == 1 && ready == 'R', "lease child did not acquire its lease");
    auto storage = open_storage(temporary.root);
    require(storage.has_value() && !storage->repository.acquire(id), "second process acquired an already-owned session lease");
    DWORD written = 0;
    const char release = 'X';
    require(WriteFile(parent_stdin_write, &release, 1, &written, nullptr) && written == 1, "failed to release lease child");
    require(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0, "lease child did not exit");
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    require(exit_code == 0, "lease child failed");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(parent_stdin_write);
    CloseHandle(parent_stdout_read);
#else
    int child_input[2]{};
    int child_output[2]{};
    require(pipe(child_input) == 0 && pipe(child_output) == 0, "failed to create lease child pipes");
    const auto child = fork();
    require(child >= 0, "failed to fork lease child");
    if (child == 0) {
        dup2(child_input[0], STDIN_FILENO);
        dup2(child_output[1], STDOUT_FILENO);
        close(child_input[1]);
        close(child_output[0]);
        execl(executable.c_str(), executable.c_str(), "--lease-child", temporary.root.c_str(), session::to_string(id).c_str(), nullptr);
        _exit(127);
    }
    close(child_input[0]);
    close(child_output[1]);
    char ready = 0;
    require(read(child_output[0], &ready, 1) == 1 && ready == 'R', "lease child did not acquire its lease");
    auto storage = open_storage(temporary.root);
    require(storage.has_value() && !storage->repository.acquire(id), "second process acquired an already-owned session lease");
    const char release = 'X';
    require(write(child_input[1], &release, 1) == 1, "failed to release lease child");
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "lease child failed");
    close(child_input[1]);
    close(child_output[0]);
#endif
}

void test_encoding_failure_precedes_begin() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    auto published =
        storage ? publish(storage->repository, make_session("valid")) : Result<Published>{lighter::outcome_error(storage.error())};
    require(published.has_value(), "failed to publish encoding fixture");
    const session::StatePaths paths{temporary.root};
    auto *blocker = open_sqlite(paths.session_database(published->value.id));
    execute(blocker, "BEGIN IMMEDIATE", "failed to hold same-session writer lock");
    const auto durable_count = published->value.entries.size();
    published->value.start_task(std::string(1, static_cast<char>(0x80)), published->value.metadata.updated_at_ms + 1);
    auto delta = session::make_delta(published->value, std::span(published->value.entries).subspan(durable_count));
    auto failed = published->writer.commit(delta);
    require(!failed && failed.error().detail.contains("not valid UTF-8"),
            "payload encoding did not fail before attempting the blocked SQLite BEGIN");
    execute(blocker, "ROLLBACK", "failed to release encoding blocker");
    sqlite3_close(blocker);
    require(scalar_i64(paths.session_database(published->value.id), "SELECT revision FROM session") == 1,
            "encoding failure changed the authoritative database");
}

void test_catalog_failure_cannot_rollback_session_and_pending_recovers() {
    TemporaryState temporary;
    session::SessionId id;
    i64 expected_recency = 0;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to open catalog failure storage");
        auto published = publish(storage->repository, make_session("initial"));
        require(published.has_value(), "failed to publish catalog failure fixture");
        id = published->value.id;
        auto *catalog_blocker = open_sqlite(session::StatePaths{temporary.root}.catalog());
        execute(catalog_blocker, "PRAGMA busy_timeout=0; BEGIN IMMEDIATE", "failed to lock catalog");
        const auto durable_count = published->value.entries.size();
        expected_recency = published->value.metadata.updated_at_ms + 10;
        published->value.start_task("durable despite catalog failure", expected_recency);
        auto committed =
            published->writer.commit(session::make_delta(published->value, std::span(published->value.entries).subspan(durable_count)));
        require(committed && !committed->catalog_degradation,
                "catalog projection remained synchronously coupled to authoritative semantic persistence");
        const session::StatePaths paths{temporary.root};
        require(scalar_i64(paths.session_database(id), "SELECT revision FROM session") == 2 &&
                    scalar_i64(paths.session_database(id), "SELECT updated_at_ms FROM session") == expected_recency &&
                    std::filesystem::exists(paths.pending_marker(id)),
                "catalog failure rolled back or lost the authoritative mutation marker");
        auto failed_refresh = published->writer.refresh_catalog();
        require(!failed_refresh && published->writer.catalog_status().degraded,
                "catalog projection failure was not exposed independently from semantic persistence");
        execute(catalog_blocker, "ROLLBACK", "failed to unlock catalog");
        sqlite3_close(catalog_blocker);
        published->value.set_model_preference("provider", "model", std::nullopt);
        auto retried = published->writer.commit(session::make_delta(published->value, {}));
        require(published->writer.refresh_catalog().has_value(), "independent catalog projection retry failed");
        auto projection = storage->catalog.find(id);
        require(retried && !retried->catalog_degradation && projection && *projection && (*projection)->observed_revision == 3 &&
                    (*projection)->summary.updated_at_ms == expected_recency && !std::filesystem::exists(paths.pending_marker(id)),
                "a later persistence commit did not retry the pending catalog projection separately");
    }
    auto repaired = open_storage(temporary.root);
    require(repaired.has_value(), "startup pending-marker reconciliation failed");
    auto projection = repaired->catalog.find(id);
    require(projection && *projection && (*projection)->observed_revision == 3 &&
                (*projection)->summary.updated_at_ms == expected_recency &&
                !std::filesystem::exists(session::StatePaths{temporary.root}.pending_marker(id)),
            "pending-marker recovery did not repair the catalog idempotently");
    write_marker(temporary.root, id, 3);
    auto repeated = open_storage(temporary.root);
    require(repeated.has_value() && !std::filesystem::exists(session::StatePaths{temporary.root}.pending_marker(id)),
            "crash after catalog upsert and before marker removal was not idempotently recoverable");
}

void test_catalog_failure_preserves_exact_authority_and_new_persistence() {
    TemporaryState temporary;
    session::SessionId existing_id;
    {
        auto storage = open_storage(temporary.root);
        auto published = storage ? publish(storage->repository, make_session("exact without catalog")) :
                                   Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish catalog-independent authority fixture");
        existing_id = published->value.id;
    }

    const session::StatePaths paths{temporary.root};
    std::error_code error;
    for (const auto &path : {paths.catalog(), std::filesystem::path(paths.catalog().string() + "-wal"),
                             std::filesystem::path(paths.catalog().string() + "-shm")}) {
        std::filesystem::remove(path, error);
        error.clear();
    }
    auto repository = session::SessionRepository::open(temporary.root, session::RepositoryOpenMode::DEFER_CATALOG_REBUILD);
    require(repository.has_value() && !std::filesystem::exists(paths.catalog()),
            "exact authoritative opening rebuilt a missing catalog eagerly");
    auto catalog = repository->catalog();
    require(!catalog, "authority-only repository exposed a deferred catalog");
    auto resolved = repository->resolve_exact(session::to_string(existing_id));
    require(resolved && *resolved == existing_id, "exact resolution depended on the corrupt catalog");
    auto existing = repository->acquire(existing_id);
    auto loaded = existing ? existing->load() : Result<session::Session>{lighter::outcome_error(existing.error())};
    require(loaded && loaded->id == existing_id, "exact authoritative acquisition depended on the corrupt catalog");

    auto fresh = make_session("persist without catalog");
    const auto fresh_id = fresh.id;
    auto writer = repository->create(fresh_id);
    require(writer.has_value(), "catalog failure prevented creating an authoritative writer");
    auto committed = writer->commit(session::make_delta(fresh, fresh.entries));
    require(committed && committed->catalog_degradation && std::filesystem::is_regular_file(paths.session_database(fresh_id)) &&
                std::filesystem::is_regular_file(paths.pending_marker(fresh_id)),
            "catalog failure promoted successful authoritative publication to semantic persistence failure");
}

void test_blocked_catalog_projection_does_not_block_semantic_commit() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open projection-isolation storage");
    auto published = publish(storage->repository, make_session("projection isolation"));
    require(published.has_value(), "failed to publish projection-isolation fixture");

    struct Handshake {
        std::mutex mutex;
        std::condition_variable changed;
        bool refresh_blocked = false;
        bool release_refresh = false;
        bool refresh_completed = false;
        bool second_commit_durable = false;
    };
    auto handshake = std::make_shared<Handshake>();
    StorageHookReset reset(storage->repository);
    session::testing::set_storage_hook(storage->repository, [handshake](session::testing::StorageEvent event) {
        std::unique_lock lock(handshake->mutex);
        if (event == session::testing::StorageEvent::CATALOG_INDEXER_BEFORE_REFRESH) {
            handshake->refresh_blocked = true;
            handshake->changed.notify_all();
            handshake->changed.wait(lock, [&] { return handshake->release_refresh; });
        } else if (event == session::testing::StorageEvent::AUTHORITATIVE_COMMIT_COMPLETED && handshake->refresh_blocked) {
            handshake->second_commit_durable = true;
            handshake->changed.notify_all();
        } else if (event == session::testing::StorageEvent::CATALOG_INDEXER_AFTER_REFRESH) {
            handshake->refresh_completed = true;
            handshake->changed.notify_all();
        }
    });

    auto &value = published->value;
    const auto first_tail = value.entries.size();
    value.start_task("first catalog-visible mutation", value.metadata.updated_at_ms + 10);
    require(published->writer.commit(session::make_delta(value, std::span(value.entries).subspan(first_tail))).has_value(),
            "failed to commit the projection-triggering mutation");
    {
        std::unique_lock lock(handshake->mutex);
        handshake->changed.wait(lock, [&] { return handshake->refresh_blocked; });
    }

    value.set_model_preference("provider", "model", std::nullopt);
    std::optional<Result<session::SessionCommitResult>> second_result;
    std::jthread second([&] { second_result = published->writer.commit(session::make_delta(value, {})); });
    {
        using namespace std::chrono_literals;
        std::unique_lock lock(handshake->mutex);
        require(handshake->changed.wait_for(lock, 5s, [&] { return handshake->second_commit_durable; }),
                "blocked catalog projection retained the session invalidation mutex across catalog I/O");
    }
    require(scalar_i64(session::StatePaths{temporary.root}.session_database(value.id), "SELECT revision FROM session") == 3,
            "second semantic commit was not durable while catalog projection was blocked");
    {
        std::scoped_lock lock(handshake->mutex);
        handshake->release_refresh = true;
    }
    handshake->changed.notify_all();
    second.join();
    require(second_result && *second_result, "second semantic commit failed after isolated catalog projection resumed");
    {
        std::unique_lock lock(handshake->mutex);
        handshake->changed.wait(lock, [&] { return handshake->refresh_completed; });
    }
}

void test_revision_guard_and_precommit_marker() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to open revision storage");
        auto value = make_session("revision guard");
        id = value.id;
        auto published = publish(storage->repository, std::move(value));
        require(published.has_value(), "failed to publish revision fixture");
        auto newer = storage->catalog.find(id);
        require(newer && *newer, "published catalog projection is absent");
        auto newest = **newer;
        newest.observed_revision = 5;
        newest.summary.title = "newer";
        require(storage->catalog.upsert(newest).has_value(), "failed to install newer projection");
        auto older = newest;
        older.observed_revision = 4;
        older.summary.title = "older";
        require(storage->catalog.upsert(older).has_value(), "guarded older upsert failed unexpectedly");
        auto guarded = storage->catalog.find(id);
        require(guarded && *guarded && (*guarded)->observed_revision == 5 && (*guarded)->summary.title == "newer",
                "older catalog refresh overwrote a newer projection");
        write_marker(temporary.root, id, 3);
        published->value.set_model_preference("provider", "model", std::nullopt);
        auto unrelated = published->writer.commit(session::make_delta(published->value, {}));
        require(unrelated.has_value() && std::filesystem::exists(session::StatePaths{temporary.root}.pending_marker(id)) &&
                    read_marker(temporary.root, id) == 3,
                "revision-2 refresh removed a marker advanced to revision 3");
    }
    auto reconciled = open_storage(temporary.root);
    require(reconciled.has_value() && !std::filesystem::exists(session::StatePaths{temporary.root}.pending_marker(id)),
            "exclusive reconciliation did not safely clear a newer pre-commit marker");
    auto authoritative = reconciled->catalog.find(id);
    require(authoritative && *authoritative && (*authoritative)->observed_revision == 5,
            "pre-commit reconciliation regressed a newer catalog projection");
}

void test_normal_startup_does_not_scan_sessions() {
    TemporaryState temporary;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to initialize no-scan storage");
    }
    const session::StatePaths paths{temporary.root};
    const auto fake = session::generate_session_id();
    std::error_code error;
    std::filesystem::create_directories(paths.session_directory(fake), error);
    require(!error, "failed to create corrupt unmarked session fixture");
    std::ofstream corrupt(paths.session_database(fake), std::ios::binary);
    corrupt << "not sqlite";
    corrupt.close();
    auto reopened = open_storage(temporary.root);
    require(reopened.has_value(), "normal startup scanned an unmarked corrupt session database");
}

void test_invalid_pending_marker_is_reported_without_scanning_sessions() {
    TemporaryState temporary;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to initialize marker-warning storage");
    }
    const auto id = session::generate_session_id();
    std::ofstream marker(session::StatePaths{temporary.root}.pending_marker(id), std::ios::binary);
    marker << "unsupported marker";
    marker.close();
    auto reopened = open_storage(temporary.root);
    require(reopened.has_value() &&
                std::ranges::any_of(reopened->repository.warnings(),
                                    [](const std::string &warning) { return warning.contains("invalid format version"); }),
            "invalid marker format was not distinguished through startup catalog warnings");
}

void test_missing_catalog_rebuild_reads_only_singletons() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        auto published =
            storage ? publish(storage->repository, make_session("rebuild me")) : Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish rebuild fixture");
        id = published->value.id;
    }
    const session::StatePaths paths{temporary.root};
    auto *database = open_sqlite(paths.session_database(id));
    execute(database, "UPDATE session_entries SET payload_version=999", "failed to corrupt payload fixture");
    sqlite3_close(database);
    std::error_code error;
    std::filesystem::remove(paths.catalog(), error);
    require(!error, "failed to remove catalog for rebuild");
    std::filesystem::remove(paths.catalog().string() + "-wal", error);
    error.clear();
    std::filesystem::remove(paths.catalog().string() + "-shm", error);
    error.clear();
    auto rebuilt = open_storage(temporary.root);
    require(rebuilt.has_value(), "missing-catalog rebuild decoded corrupt entry payloads");
    auto page = rebuilt->catalog.page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 1 && page->sessions.front().id == id,
            "singleton-only rebuild did not reproduce the discoverable session");
    auto writer = rebuilt->repository.acquire(id);
    require(writer.has_value() && !writer->load(), "full session load unexpectedly accepted the corrupt payload");
}

void test_catalog_ingestion_rejects_invalid_singleton_projections() {
    TemporaryState temporary;
    session::SessionId valid_id;
    session::SessionId invalid_id;
    {
        auto storage = open_storage(temporary.root);
        auto valid = storage ? publish(storage->repository, make_session("valid projection")) :
                               Result<Published>{lighter::outcome_error(storage.error())};
        auto invalid = storage ? publish(storage->repository, make_session("invalid projection")) :
                                 Result<Published>{lighter::outcome_error(storage.error())};
        require(valid && invalid, "failed to publish projection-validation fixtures");
        valid_id = valid->value.id;
        invalid_id = invalid->value.id;

        session::CatalogProjection projection{
            .summary = {.id = session::generate_session_id(), .updated_at_ms = 1, .preview = "valid"},
            .observed_revision = 1,
            .workspace_key = "workspace",
        };
        projection.observed_revision = 0;
        require(!storage->catalog.upsert(projection), "catalog accepted a zero authoritative revision");
        projection.observed_revision = 1;
        projection.workspace_key.clear();
        require(!storage->catalog.upsert(projection), "catalog accepted an empty workspace identity");
        projection.workspace_key = "workspace";
        projection.summary.updated_at_ms = 0;
        require(!storage->catalog.upsert(projection), "catalog accepted an invalid conversation timestamp");
        projection.summary.updated_at_ms = 1;
        projection.summary.title = "";
        require(!storage->catalog.upsert(projection), "catalog accepted an empty title");
        projection.summary.title.reset();
        projection.summary.preview = std::string(1, static_cast<char>(0x80));
        require(!storage->catalog.upsert(projection), "catalog accepted invalid UTF-8 preview metadata");
    }

    const session::StatePaths paths{temporary.root};
    auto *database = open_sqlite(paths.session_database(invalid_id));
    execute(database, "UPDATE session SET preview=CAST(X'80' AS TEXT)", "failed to corrupt authoritative projection fixture");
    sqlite3_close(database);
    std::error_code error;
    for (const auto &path : {paths.catalog(), std::filesystem::path(paths.catalog().string() + "-wal"),
                             std::filesystem::path(paths.catalog().string() + "-shm")}) {
        std::filesystem::remove(path, error);
        error.clear();
    }
    auto rebuilt = open_storage(temporary.root);
    require(rebuilt.has_value(), "invalid authoritative projection prevented catalog reconstruction");
    auto valid = rebuilt->catalog.find(valid_id);
    auto invalid = rebuilt->catalog.find(invalid_id);
    require(valid && *valid && invalid && !*invalid &&
                std::ranges::any_of(rebuilt->repository.warnings(),
                                    [](const std::string &warning) { return warning.contains("invalid preview"); }),
            "catalog rebuild did not report and exclude an invalid authoritative singleton projection");
}

void test_missing_catalog_rebuild_includes_leased_sessions() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        auto published = storage ? publish(storage->repository, make_session("leased rebuild")) :
                                   Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish leased rebuild fixture");
        id = published->value.id;
    }
    auto lease = session::acquire_session_lease(temporary.root, id);
    require(lease.has_value(), "failed to hold rebuild fixture lease");
    const session::StatePaths paths{temporary.root};
    std::error_code error;
    std::filesystem::remove(paths.catalog(), error);
    require(!error, "failed to remove catalog for leased rebuild");
    std::filesystem::remove(paths.catalog().string() + "-wal", error);
    error.clear();
    std::filesystem::remove(paths.catalog().string() + "-shm", error);
    error.clear();
    auto rebuilt = open_storage(temporary.root);
    require(rebuilt.has_value(), "missing-catalog rebuild failed while a session was leased");
    auto page = rebuilt->catalog.page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 1 && page->sessions.front().id == id,
            "missing-catalog rebuild omitted an exclusively owned session");
}

void test_incomplete_catalog_rebuild_is_resumed_after_crash() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        auto published = storage ? publish(storage->repository, make_session("durable rebuild marker")) :
                                   Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish rebuild-marker fixture");
        id = published->value.id;
    }
    const session::StatePaths paths{temporary.root};
    std::error_code error;
    for (const auto &path : {paths.catalog(), std::filesystem::path(paths.catalog().string() + "-wal"),
                             std::filesystem::path(paths.catalog().string() + "-shm")}) {
        std::filesystem::remove(path, error);
        error.clear();
    }

    {
        auto *blocker = open_sqlite(paths.catalog());
        execute(blocker, R"sql(
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
PRAGMA journal_mode = WAL;
)sql",
                "failed to create interrupted catalog fixture");
        std::ofstream(paths.catalog_rebuild_marker(), std::ios::binary) << "1\n";
        require(std::filesystem::is_regular_file(paths.catalog_rebuild_marker()), "failed to record interrupted catalog rebuild fixture");
        execute(blocker, "PRAGMA busy_timeout=0; BEGIN IMMEDIATE", "failed to block interrupted catalog rebuild");
        auto failed = session::SessionRepository::open(temporary.root);
        require(failed && !failed->catalog() && std::filesystem::is_regular_file(paths.catalog_rebuild_marker()),
                "failed catalog rebuild did not leave authority available with a degraded catalog");
        execute(blocker, "ROLLBACK", "failed to release interrupted catalog rebuild");
        sqlite3_close(blocker);
    }
    auto resumed = open_storage(temporary.root);
    require(resumed.has_value(), "next opener did not resume an interrupted catalog rebuild");
    auto page = resumed->catalog.page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 1 && page->sessions.front().id == id &&
                !std::filesystem::exists(paths.catalog_rebuild_marker()),
            "resumed catalog rebuild did not restore discovery before clearing its durable marker");
}

void test_concurrent_catalog_initialization_is_serialized() {
    TemporaryState temporary;
    constexpr usize opener_count = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(opener_count));
    std::array<std::atomic<bool>, opener_count> opened{};
    std::array<std::string, opener_count> errors;
    std::vector<std::jthread> threads;
    threads.reserve(opener_count);
    for (usize index = 0; index < opener_count; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            auto storage = open_storage(temporary.root);
            opened[index] = storage.has_value();
            if (!storage) errors[index] = storage.error().message();
        });
    }
    threads.clear();
    for (usize index = 0; index < opener_count; ++index) {
        if (!opened[index]) fail("concurrent first catalog opener failed: " + errors[index]);
    }
    auto storage = open_storage(temporary.root);
    require(storage.has_value() && !std::filesystem::exists(session::StatePaths{temporary.root}.catalog_rebuild_marker()),
            "concurrently initialized catalog did not finish its serialized rebuild");
}

void test_empty_catalog_creation_crash_is_recovered_exclusively() {
    TemporaryState temporary;
    std::ofstream(session::StatePaths{temporary.root}.catalog(), std::ios::binary).close();
    auto storage = open_storage(temporary.root);
    require(storage.has_value() && !std::filesystem::exists(session::StatePaths{temporary.root}.catalog_rebuild_marker()),
            "empty catalog left before its rebuild marker was not safely initialized");
}

void test_corrupt_catalog_repair_requires_exclusive_maintenance() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        auto published = storage ? publish(storage->repository, make_session("repair catalog")) :
                                   Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish corrupt-catalog fixture");
        id = published->value.id;
        auto refused = session::SessionRepository::repair_catalog(temporary.root);
        require(!refused && refused.error().detail.contains("every Liminal process"),
                "catalog replacement ignored an existing catalog owner");
    }
    const session::StatePaths paths{temporary.root};
    std::error_code error;
    std::filesystem::remove(paths.catalog(), error);
    require(!error, "failed to remove catalog before corruption fixture");
    std::ofstream corrupt(paths.catalog(), std::ios::binary);
    corrupt << "foreign corrupt bytes";
    corrupt.close();
    auto degraded = session::SessionRepository::open(temporary.root);
    require(degraded && !degraded->catalog(), "corrupt catalog was silently adopted");
    auto repository = session::SessionRepository::repair_catalog(temporary.root);
    require(repository.has_value(), "exclusive corrupt-catalog replacement and rebuild failed");
    auto repaired_catalog = repository->catalog();
    require(repaired_catalog.has_value(), "repaired repository did not expose its rebuilt catalog");
    auto page = repaired_catalog->page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 1 && page->sessions.front().id == id,
            "corrupt-catalog repair did not rebuild published session discovery");
}

void test_missing_catalog_recreation_requires_exclusive_maintenance() {
#ifndef _WIN32
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open missing-catalog ownership fixture");
    const session::StatePaths paths{temporary.root};
    std::error_code error;
    for (const auto &path : {paths.catalog(), std::filesystem::path(paths.catalog().string() + "-wal"),
                             std::filesystem::path(paths.catalog().string() + "-shm")}) {
        std::filesystem::remove(path, error);
        error.clear();
    }
    require(!std::filesystem::exists(paths.catalog()), "failed to unlink the live POSIX catalog fixture");
    auto replacement = session::SessionRepository::open(temporary.root);
    auto replacement_catalog =
        replacement ? replacement->catalog() : Result<session::SessionCatalog>{lighter::outcome_error(replacement.error())};
    require(replacement && !replacement_catalog && replacement_catalog.error().detail.contains("every Liminal process") &&
                !std::filesystem::exists(paths.catalog()),
            "missing catalog contention did not preserve authority-only repository access");
#endif
}

void test_staging_cancellation_and_complete_publication() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open staging storage");
    auto value = make_session("staged fork");
    const auto id = value.id;
    const auto delta = session::make_delta(value, value.entries);
    {
        auto staged = storage->repository.stage(id, delta);
        require(staged.has_value() && !std::filesystem::exists(session::StatePaths{temporary.root}.session_directory(id)),
                "staging exposed an unpublished session");
        const auto staging_directory = *std::filesystem::directory_iterator(session::StatePaths{temporary.root}.staging());
        require(std::filesystem::is_regular_file(staging_directory.path() / "session.sqlite3") &&
                    !std::filesystem::exists(staging_directory.path() / "session.sqlite3-wal"),
                "staging used or retained a live WAL sidecar");
    }
    require(directory_empty(session::StatePaths{temporary.root}.staging()) &&
                !std::filesystem::exists(session::StatePaths{temporary.root}.session_directory(id)),
            "cancelling an unpublished staged session left a durable artifact");
    auto staged = storage->repository.stage(id, delta);
    require(staged.has_value(), "failed to restage publication fixture");
    value.metadata.updated_at_ms += 25;
    auto published = staged->commit(session::make_delta(value, value.entries));
    require(published.has_value() && scalar_text(session::StatePaths{temporary.root}.session_database(id), "PRAGMA journal_mode") == "wal",
            "complete staged image did not publish and reopen in verified WAL mode");
}

void test_publication_retries_across_rename_and_reopen_boundaries() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open publication-boundary storage");
    constexpr std::array failures{
        session::testing::StorageFailure::PUBLICATION_BEFORE_RENAME,
        session::testing::StorageFailure::PUBLICATION_AFTER_RENAME,
        session::testing::StorageFailure::PUBLICATION_AFTER_REOPEN,
    };
    for (const auto failure : failures) {
        auto value = make_session("retry atomic publication");
        auto writer = storage->repository.create(value.id);
        require(writer.has_value(), "failed to create publication-boundary writer");
        const auto initial = session::make_delta(value, value.entries);
        session::testing::fail_storage_once(storage->repository, failure);
        auto first = writer->commit(initial);
        require(!first, "publication failure injection did not interrupt its boundary");
        const auto published = failure != session::testing::StorageFailure::PUBLICATION_BEFORE_RENAME;
        require(std::filesystem::is_regular_file(session::StatePaths{temporary.root}.session_database(value.id)) == published,
                "publication failure crossed the wrong side of the atomic rename");
        if (published) {
            auto changed = initial;
            changed.metadata.updated_at_ms += 1;
            changed.metadata.title = "different finalized snapshot";
            changed.next_task_id += 1;
            auto rejected = writer->commit(changed);
            require(!rejected && rejected.error().detail.contains("does not match"),
                    "post-rename attachment acknowledged a delta other than the finalized snapshot");
        }

        auto retried = writer->commit(initial);
        auto loaded = retried ? writer->load() : Result<session::Session>{lighter::outcome_error(retried.error())};
        auto projection = storage->catalog.find(value.id);
        require(retried && loaded && loaded->entries.size() == value.entries.size() && projection && *projection &&
                    (*projection)->observed_revision == 1,
                "retry did not attach and project the already-published authoritative database exactly once");
    }
}

void test_concurrent_publication_revalidates_materialization_under_lock() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open concurrent-publication storage");
    auto value = make_session("concurrent publication");
    auto writer = storage->repository.stage(value.id, session::make_delta(value, value.entries));
    require(writer.has_value(), "failed to stage concurrent-publication fixture");
    const auto initial = session::make_delta(value, value.entries);

    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        usize snapshots = 0;
        bool release = false;
    };
    auto gate = std::make_shared<Gate>();
    StorageHookReset reset(storage->repository);
    session::testing::set_storage_hook(storage->repository, [gate](session::testing::StorageEvent event) {
        if (event != session::testing::StorageEvent::PUBLICATION_STATE_SNAPSHOTTED) return;
        std::unique_lock lock(gate->mutex);
        ++gate->snapshots;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
    });

    std::array<std::optional<Result<session::SessionCommitResult>>, 2> results;
    std::array<std::jthread, 2> publishers{
        std::jthread([&] { results[0] = writer->commit(initial); }),
        std::jthread([&] { results[1] = writer->commit(initial); }),
    };
    {
        std::unique_lock lock(gate->mutex);
        gate->changed.wait(lock, [&] { return gate->snapshots == 2; });
        gate->release = true;
    }
    gate->changed.notify_all();
    for (auto &publisher : publishers) publisher.join();
    const auto succeeded = std::ranges::count_if(results, [](const auto &result) { return result && result->has_value(); });
    const auto rejected = std::ranges::count_if(
        results, [](const auto &result) { return result && !*result && result->error().detail.contains("concurrent publication"); });
    auto loaded = writer->load();
    require(succeeded == 1 && rejected == 1 && loaded && loaded->entries.size() == value.entries.size(),
            "concurrent staged commits did not publish exactly once and reject stale materialization");
}

void test_unpublished_queue_recovers_post_rename_attachment() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open unpublished-retry storage");
    auto value = make_session("recover fork publication");
    auto writer = storage->repository.stage(value.id, session::make_delta(value, value.entries));
    require(writer.has_value(), "failed to stage unpublished-retry fixture");
    auto queue = session::PersistenceQueue::create_unpublished(*std::move(writer));
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        usize snapshots = 0;
        bool retry_blocked = false;
        bool release = false;
    };
    auto gate = std::make_shared<Gate>();
    StorageHookReset reset(storage->repository);
    session::testing::set_storage_hook(storage->repository, [gate](session::testing::StorageEvent event) {
        if (event != session::testing::StorageEvent::PUBLICATION_STATE_SNAPSHOTTED) return;
        std::unique_lock lock(gate->mutex);
        ++gate->snapshots;
        if (gate->snapshots == 1) return;
        gate->retry_blocked = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
    });
    session::testing::fail_storage_once(storage->repository, session::testing::StorageFailure::PUBLICATION_AFTER_RENAME);
    auto published = queue->publish_initial(session::make_delta(value, value.entries));
    {
        std::unique_lock lock(gate->mutex);
        gate->changed.wait(lock, [&] { return gate->retry_blocked; });
    }
    require(published && *published == session::InitialPublicationStatus::ATTACHMENT_PENDING && queue->status().degraded &&
                queue->status().publication_attachment_pending,
            "post-rename publication failure was not represented as recoverable published authority");
    {
        std::scoped_lock lock(gate->mutex);
        gate->release = true;
    }
    gate->changed.notify_all();
    require(queue->flush().has_value(), "unpublished queue did not retry attachment of the same finalized snapshot");
    auto projection = storage->catalog.find(value.id);
    require(!queue->status().degraded && projection && *projection && (*projection)->observed_revision == 1,
            "attachment retry did not recover the published fork and its catalog projection");
}

void test_abandoned_staging_and_marker_are_reconciled() {
    TemporaryState temporary;
    {
        auto storage = open_storage(temporary.root);
        require(storage.has_value(), "failed to initialize abandoned staging storage");
    }
    const session::StatePaths paths{temporary.root};
    const auto id = session::generate_session_id();
    const auto staging = paths.staging() / (session::to_string(id) + ".deadbeef");
    std::error_code error;
    std::filesystem::create_directory(staging, error);
    require(!error, "failed to create abandoned staging fixture");
    std::ofstream database(staging / "session.sqlite3", std::ios::binary);
    database << "closed staging image";
    database.close();
    write_marker(temporary.root, id, 1);
    auto reopened = open_storage(temporary.root);
    require(reopened.has_value() && !std::filesystem::exists(staging) && !std::filesystem::exists(paths.pending_marker(id)),
            "startup did not clean lease-free abandoned staging and its orphan marker");
}

void test_staged_final_snapshot_is_fully_validated() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open staged validation storage");
    const auto rejected = [&](auto mutate, std::string_view message) {
        auto value = make_session("staged validation");
        const auto id = value.id;
        auto staged = storage->repository.stage(id, session::make_delta(value, value.entries));
        require(staged.has_value(), "failed to stage validation fixture");
        auto final = session::make_delta(value, value.entries);
        mutate(final);
        require(!staged->commit(final) && !std::filesystem::exists(session::StatePaths{temporary.root}.session_directory(id)), message);
    };
    rejected([](session::SessionDelta &delta) { delta.metadata.updated_at_ms = delta.metadata.created_at_ms - 1; },
             "staged publication accepted regressed timestamps");
    rejected([](session::SessionDelta &delta) { delta.next_task_id = 0; }, "staged publication accepted regressed counters");
    rejected([](session::SessionDelta &delta) { delta.active_leaf = session::EntryId{delta.entry_count + 1}; },
             "staged publication accepted an invalid active leaf");
    rejected([](session::SessionDelta &delta) { delta.metadata.title = std::string(1, static_cast<char>(0x80)); },
             "staged publication accepted malformed title UTF-8");
    rejected(
        [](session::SessionDelta &delta) {
            delta.metadata.model_preference = session::SessionModelPreference{.provider = "", .model = "model"};
        },
        "staged publication accepted invalid model metadata");
    rejected(
        [](session::SessionDelta &delta) {
            delta.metadata.forked_from = session::ForkOrigin{.session = session::generate_session_id(), .entry = {0}};
        },
        "staged publication accepted invalid fork metadata");

    const auto rejected_relative = [&](auto regress, std::string_view message) {
        auto value = make_session("staged relative validation");
        auto initial = session::make_delta(value, value.entries);
        initial.metadata.updated_at_ms += 100;
        initial.next_task_id += 10;
        initial.next_provider_call_id += 10;
        initial.tokens_used += 100;
        initial.metadata.forked_from = session::ForkOrigin{.session = session::generate_session_id(), .entry = {1}};
        auto staged = storage->repository.stage(value.id, initial);
        require(staged.has_value(), "failed to stage relative-validation fixture");
        auto final = initial;
        regress(final);
        require(!staged->commit(final) && !std::filesystem::exists(session::StatePaths{temporary.root}.session_directory(value.id)),
                message);
    };
    rejected_relative([](session::SessionDelta &delta) { --delta.metadata.updated_at_ms; },
                      "staged publication accepted updated_at regression relative to its staged baseline");
    rejected_relative([](session::SessionDelta &delta) { --delta.next_task_id; },
                      "staged publication accepted task-counter regression relative to its staged baseline");
    rejected_relative([](session::SessionDelta &delta) { --delta.next_provider_call_id; },
                      "staged publication accepted provider-counter regression relative to its staged baseline");
    rejected_relative([](session::SessionDelta &delta) { --delta.tokens_used; },
                      "staged publication accepted token-count regression relative to its staged baseline");
    rejected_relative(
        [](session::SessionDelta &delta) {
            delta.metadata.forked_from = session::ForkOrigin{.session = session::generate_session_id(), .entry = {1}};
        },
        "staged publication accepted changed fork provenance relative to its staged baseline");
}

void test_recency_rename_fork_and_discovery() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open recency storage");
    auto first = publish(storage->repository, make_session("first prompt"));
    auto second = publish(storage->repository, make_session("second prompt"));
    require(first && second, "failed to publish discovery fixtures");
    auto page = storage->catalog.page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 2, "not every published session participates in workspace discovery");
    const auto before = first->value.metadata.updated_at_ms;
    first->value.set_title("renamed");
    auto renamed = first->writer.commit(session::make_delta(first->value, {}));
    require(renamed && first->value.metadata.updated_at_ms == before, "rename advanced authoritative conversation recency");
    auto renamed_projection = storage->catalog.find(first->value.id);
    require(renamed_projection && *renamed_projection && (*renamed_projection)->summary.title == "renamed" &&
                (*renamed_projection)->summary.updated_at_ms == before,
            "rename did not synchronously refresh the bounded selector projection");

    const auto task = first->value.entries.front().id;
    first->value.append(session::TaskFinished{.id = {1}});
    auto fork = first->value.fork_at({.entry = first->value.entries.back().id});
    require(fork.has_value(), "failed to create fork prefix");
    const auto fork_id = fork->id;
    auto staged = storage->repository.stage(fork_id, session::make_delta(*fork, fork->entries));
    require(staged.has_value(), "failed to stage fork prefix");
    const auto publication_time = std::max(fork->metadata.updated_at_ms + 100, session::unix_milliseconds_now());
    fork->metadata.updated_at_ms = publication_time;
    require(staged->commit(session::make_delta(*fork, fork->entries)).has_value(), "fork publication failed");
    auto fork_projection = storage->catalog.find(fork_id);
    require(fork_projection && *fork_projection && (*fork_projection)->summary.updated_at_ms == publication_time,
            "fork recency did not use publication time");
    static_cast<void>(task);
}

void test_restart_branching_preserves_history_and_cursor() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open restart-branching storage");
    auto value = make_session("branch root");
    const auto root = value.append(session::TaskFinished{.id = {1}});
    const auto old_task = value.start_task("old descendant", value.metadata.updated_at_ms + 1);
    const auto original_leaf = value.append(session::TaskFinished{.id = old_task});
    auto published = publish(storage->repository, std::move(value));
    require(published.has_value(), "failed to seed restart-branching history");
    require(published->value.checkout(checkpoint_id(root)).has_value(), "failed to select restart branch root");
    require(published->writer.commit(session::make_delta(published->value, {})).has_value(), "failed to persist active cursor");
    auto cursor_restart = published->writer.load();
    require(cursor_restart && cursor_restart->active_leaf == root, "active cursor did not survive restart");
    const auto first_new_entry = published->value.entries.size();
    const auto alternate = published->value.start_task("alternate", published->value.metadata.updated_at_ms + 1);
    published->value.append(session::TaskFinished{.id = alternate});
    require(published->writer.commit(session::make_delta(published->value, std::span(published->value.entries).subspan(first_new_entry)))
                .has_value(),
            "failed to persist alternate restart branch");
    auto restarted = published->writer.load();
    require(restarted && restarted->find(original_leaf) && restarted->entries[first_new_entry].parent_id == root,
            "restart branching rewrote the preserved descendant or lost the selected parent");
}

void test_catalog_keyset_paging_and_index_plan() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open catalog paging storage");
    insert_catalog_rows(storage->catalog.path(), 1, 2, "workspace-a", 100, 100);
    insert_catalog_rows(storage->catalog.path(), 3, 2, "workspace-a", 300, 0);
    insert_catalog_rows(storage->catalog.path(), 100, 1, "workspace-b", 1'000);
    auto first = storage->catalog.page({.workspace_key = "workspace-a", .limit = 2});
    require(first && first->sessions.size() == 2 && first->sessions[0].id == deterministic_id(4) &&
                first->sessions[1].id == deterministic_id(3) && first->continuation,
            "first keyset page did not use stable newest-first ID tie-breaking");
    auto second = storage->catalog.page({.workspace_key = "workspace-a", .after = first->continuation, .limit = 2});
    require(second && second->sessions.size() == 2 && second->sessions[0].id == deterministic_id(2) &&
                second->sessions[1].id == deterministic_id(1) && !second->continuation,
            "continued keyset page skipped or duplicated catalog rows");
    auto other = storage->catalog.page({.workspace_key = "workspace-b", .limit = 2});
    require(other && other->sessions.size() == 1 && other->sessions.front().id == deterministic_id(100),
            "catalog keyset paging crossed workspace boundaries");

    auto *database = open_sqlite(storage->catalog.path());
    sqlite3_stmt *plan = nullptr;
    constexpr auto sql = R"sql(
EXPLAIN QUERY PLAN SELECT id,updated_at_ms,title,preview FROM sessions
WHERE workspace_key=?1 AND (updated_at_ms,id)<(?2,?3)
ORDER BY updated_at_ms DESC,id DESC LIMIT ?4
)sql";
    require(sqlite3_prepare_v2(database, sql, -1, &plan, nullptr) == SQLITE_OK, "failed to prepare catalog query plan");
    sqlite3_bind_text(plan, 1, "workspace-a", -1, SQLITE_STATIC);
    sqlite3_bind_int64(plan, 2, 300);
    const auto cursor_id = deterministic_id(3);
    sqlite3_bind_blob(plan, 3, cursor_id.bytes.data(), static_cast<int>(cursor_id.bytes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(plan, 4, 3);
    std::string details;
    while (sqlite3_step(plan) == SQLITE_ROW) {
        details += reinterpret_cast<const char *>(sqlite3_column_text(plan, 3));
        details += '\n';
    }
    sqlite3_finalize(plan);
    sqlite3_close(database);
    require(details.contains("sessions_workspace_recent") && !details.contains("USE TEMP B-TREE"),
            "catalog paging did not use the workspace/recency keyset index directly");
}

void test_recovery_and_transcript_hydration_survive_restart() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open recovery storage");
    auto value = make_session("run tools");
    const auto task = session::TaskId{1};
    const auto call = value.next_provider_call();
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::AssistantMessageItem{.id = {.value = "commentary"},
                                               .parts = {{.text = "checking"}},
                                               .phase = provider::MessagePhase::COMMENTARY},
    });
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "tool"}, .call = tool_call("durable-call")},
    });
    auto published = publish(storage->repository, std::move(value));
    require(published.has_value(), "failed to persist interrupted recovery prefix");
    auto restarted = published->writer.load();
    require(restarted.has_value(), "failed to reload interrupted recovery prefix");
    const auto durable_size = restarted->entries.size();
    auto recovery = session::recover_interrupted(*restarted);
    require(recovery.unknown_tool_outcomes == 1, "recovery did not synthesize the unmatched durable tool outcome");
    auto suffix = std::span(restarted->entries).subspan(durable_size);
    require(published->writer.commit(session::make_delta(*restarted, suffix)).has_value(), "failed to persist recovery suffix");
    auto recovered = published->writer.load();
    require(recovered && std::get<session::TaskFinished>(recovered->entries.back().payload).outcome == session::TaskOutcome::INTERRUPTED,
            "interrupted recovery suffix did not survive restart");
    ToolSet tools(temporary.root);
    auto blocks = tui::project_transcript(*recovered, tools);
    require(blocks.size() >= 5 && blocks.front().kind == tui::BlockKind::USER &&
                std::ranges::any_of(blocks, [](const tui::Block &block) { return block.kind == tui::BlockKind::TOOL; }) &&
                blocks.back().text.contains("interrupted"),
            "transcript hydration did not reconstruct recovered user, tool, and interruption blocks");
}

void test_persistence_queue_ordering_retry_and_flush_barriers() {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        usize attempts = 0;
        bool release_failure = false;
        bool release_success = false;
        std::vector<session::SessionDelta> received;
    } gate;
    session::Session value;
    auto queue = session::PersistenceQueue::create_for_test(value.id, [&gate](const session::SessionDelta &delta) -> Result<void> {
        std::unique_lock lock(gate.mutex);
        ++gate.attempts;
        gate.received.push_back(delta);
        gate.changed.notify_all();
        if (gate.attempts == 1) {
            gate.changed.wait(lock, [&gate] { return gate.release_failure; });
            return lighter::outcome_error(Error::storage("injected save failure"));
        }
        gate.changed.wait(lock, [&gate] { return gate.release_success; });
        return {};
    });
    require(value.attach_persistence(queue).has_value(), "failed to attach ordered persistence queue");
    const auto task = value.start_task("queue test");
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 1; });
    }
    value.append(session::TaskFinished{.id = task});
    {
        std::scoped_lock lock(gate.mutex);
        gate.release_failure = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 2; });
        require(queue->status().degraded && queue->status().pending_mutations == 2,
                "failed persistence did not retain its complete ordered tail");
        gate.release_success = true;
    }
    gate.changed.notify_all();
    require(queue->flush().has_value(), "ordered persistence retry did not flush its complete prefix");
    std::scoped_lock lock(gate.mutex);
    require(gate.received.back().entries.size() == 2 && gate.received.back().entries[1].parent_id == gate.received.back().entries[0].id,
            "ordered persistence retry changed semantic order or parent links");
}

void test_reopening_queue_tracks_asynchronous_catalog_recovery() {
    TemporaryState temporary;
    const session::StatePaths paths{temporary.root};
    struct Attempts {
        std::mutex mutex;
        std::condition_variable changed;
        usize completed = 0;
    } attempts;
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "C:/workspace", .key = "workspace"};
    value.metadata.working_directory = "C:/workspace";
    auto queue = session::testing::create_reopening_queue(temporary.root, value.id, "initial storage failure",
                                                          [&](session::testing::StorageEvent event) {
                                                              if (event != session::testing::StorageEvent::CATALOG_INDEXER_AFTER_REFRESH)
                                                                  return;
                                                              {
                                                                  std::scoped_lock lock(attempts.mutex);
                                                                  ++attempts.completed;
                                                              }
                                                              attempts.changed.notify_all();
                                                          });
    require(value.attach_persistence(queue).has_value(), "failed to attach reopening persistence queue");
    value.start_task("recover storage", std::max<i64>(1'000'000, value.metadata.created_at_ms));
    require(queue->flush().has_value(), "reopening queue did not recover semantic persistence");

    auto *catalog_blocker = open_sqlite(paths.catalog());
    execute(catalog_blocker, "PRAGMA busy_timeout=0; BEGIN IMMEDIATE", "failed to block recovery catalog");

    value.set_title("catalog failure after recovery");
    require(queue->flush().has_value(), "catalog failure was promoted to reopening semantic persistence failure");
    {
        std::unique_lock lock(attempts.mutex);
        attempts.changed.wait(lock, [&] { return attempts.completed >= 1; });
    }
    require(queue->status().catalog_degraded, "reopening queue did not expose its recovered writer's catalog failure");

    execute(catalog_blocker, "ROLLBACK", "failed to unblock recovery catalog");
    sqlite3_close(catalog_blocker);
    {
        std::unique_lock lock(attempts.mutex);
        attempts.changed.wait(lock, [&] { return attempts.completed >= 2; });
    }
    require(!queue->status().catalog_degraded,
            "reopening queue retained stale catalog degradation after its recovered writer's asynchronous retry succeeded");
}

void test_flush_waits_for_complete_pending_prefix() {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        usize attempts = 0;
        bool release_first = false;
        bool release_second = false;
        bool flush_returned = false;
        bool flush_succeeded = false;
    } gate;
    session::Session value;
    auto queue = session::PersistenceQueue::create_for_test(value.id, [&gate](const session::SessionDelta &) -> Result<void> {
        std::unique_lock lock(gate.mutex);
        ++gate.attempts;
        const auto attempt = gate.attempts;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate, attempt] { return attempt == 1 ? gate.release_first : gate.release_second; });
        return {};
    });
    require(value.attach_persistence(queue).has_value(), "failed to attach flush-barrier queue");
    const auto task = value.start_task("first mutation");
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 1; });
    }
    value.append(session::TaskFinished{.id = task});
    std::jthread flusher([&] {
        const auto flushed = queue->flush();
        std::scoped_lock lock(gate.mutex);
        gate.flush_returned = true;
        gate.flush_succeeded = flushed.has_value();
        gate.changed.notify_all();
    });
    {
        std::scoped_lock lock(gate.mutex);
        gate.release_first = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 2; });
        require(!gate.flush_returned, "flush returned after only part of its pending prefix committed");
        gate.release_second = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.flush_returned; });
        require(gate.flush_succeeded, "flush failed after its complete pending prefix committed");
    }
}

void test_initial_publication_serializes_enqueued_mutations() {
    struct Gate {
        std::latch initial_entered{1};
        std::latch release_initial{1};
        std::atomic<usize> attempts = 0;
        std::atomic<usize> active = 0;
        std::atomic<usize> maximum_active = 0;
        std::mutex mutex;
        std::vector<session::SessionDelta> received;
    } gate;
    session::Session value;
    const auto initial = session::make_delta(value, value.entries);
    auto queue =
        session::PersistenceQueue::create_unpublished_for_test(value.id, [&gate](const session::SessionDelta &delta) -> Result<void> {
            const auto attempt = gate.attempts.fetch_add(1) + 1;
            const auto active = gate.active.fetch_add(1) + 1;
            auto maximum = gate.maximum_active.load();
            while (maximum < active && !gate.maximum_active.compare_exchange_weak(maximum, active)) {}
            {
                std::scoped_lock lock(gate.mutex);
                gate.received.push_back(delta);
            }
            if (attempt == 1) {
                gate.initial_entered.count_down();
                gate.release_initial.wait();
            }
            gate.active.fetch_sub(1);
            return {};
        });
    require(value.attach_persistence(queue).has_value(), "failed to attach unpublished persistence queue");
    std::atomic<bool> published = false;
    std::jthread publisher([&] { published = queue->publish_initial(initial).has_value(); });
    gate.initial_entered.wait();
    value.start_task("queued during publication");
    require(queue->status().pending_mutations == 1, "publication lost a concurrently enqueued mutation");
    gate.release_initial.count_down();
    publisher.join();
    require(published && queue->flush().has_value() && gate.attempts == 2 && gate.maximum_active == 1,
            "initial publication overlapped or reordered its queued semantic suffix");
    require(!queue->publish_initial(initial), "initial publication queue accepted a second publication");
}

void test_exact_open_bypasses_catalog_and_failed_hint_preserves_live_session() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open selection storage");
    auto published = publish(storage->repository, make_session("exact"));
    require(published.has_value(), "failed to publish exact-open fixture");
    const auto id = published->value.id;
    require(storage->catalog.remove(id).has_value(), "failed to remove exact-open catalog hint");
    auto exact = storage->repository.resolve_exact(session::to_string(id));
    require(exact && *exact == id, "missing catalog row prevented deterministic exact-ID resolution");

    session::Session live = make_session("live remains");
    const auto live_id = live.id;
    const auto live_entries = live.entries.size();
    const auto missing = session::generate_session_id();
    require(storage->catalog
                .upsert({.summary = {.id = missing, .updated_at_ms = live.metadata.updated_at_ms, .preview = "missing"},
                         .observed_revision = 1,
                         .workspace_key = "workspace"})
                .has_value(),
            "failed to create stale selection hint");
    application::SessionPreparationServices services(
        [](const session::Session &) -> Result<std::vector<tui::Block>> { return std::vector<tui::Block>{}; },
        [](const std::optional<session::SessionModelPreference> &) -> Result<application::SessionModelResolution> {
            return lighter::outcome_error(Error::config("model resolution should not run for a missing session"));
        });
    application::SessionCoordinator coordinator(storage->repository, std::move(services));
    auto switching = coordinator.begin_switch(live.id, missing);
    require(!switching && live.id == live_id && live.entries.size() == live_entries,
            "failed catalog-hint acquisition mutated the currently active session");

    session::SessionId authority_id;
    {
        auto authoritative = publish(storage->repository, make_session("workspace authority"));
        require(authoritative.has_value(), "failed to publish workspace-authority fixture");
        authority_id = authoritative->value.id;
        auto stale = storage->catalog.find(authority_id);
        require(stale && *stale, "workspace-authority catalog row is absent");
        auto stale_projection = **stale;
        stale_projection.workspace_key = "stale-workspace";
        require(storage->catalog.upsert(stale_projection).has_value(), "failed to install stale workspace hint");
    }
    auto mismatched = coordinator.begin_switch(live.id, authority_id);
    auto repaired = storage->catalog.find(authority_id);
    require(!mismatched && live.id == live_id && repaired && *repaired && (*repaired)->workspace_key == "workspace",
            "workspace mismatch changed the live session or deleted rather than repaired its valid catalog projection");
    auto continued = coordinator.acquire_in_workspace(authority_id, "stale-workspace");
    require(!continued, "workspace-scoped acquisition trusted stale catalog authority");

    const auto stale_latest = session::generate_session_id();
    require(storage->catalog
                .upsert({.summary = {.id = stale_latest, .updated_at_ms = 9'000'000'000'000, .preview = "missing latest"},
                         .observed_revision = 1,
                         .workspace_key = "workspace"})
                .has_value(),
            "failed to install stale latest-session hint");
    auto latest = coordinator.acquire_latest("workspace");
    auto removed = storage->catalog.find(stale_latest);
    if (!latest) fail("latest-session acquisition failed after stale hint removal: " + latest.error().message());
    require(latest->session.id == authority_id, "latest-session acquisition did not continue to the next resumable authority");
    require(removed && !*removed, "latest-session acquisition did not remove its missing catalog hint");
}

void test_prepared_switch_keeps_live_state_atomic() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open atomic-switch storage");
    session::SessionId target_id;
    {
        auto target = publish(storage->repository, make_session("switch target"));
        require(target.has_value(), "failed to publish atomic-switch target");
        target_id = target->value.id;
    }
    auto live = make_session("live state");
    const auto live_id = live.id;
    const auto live_entries = live.entries.size();
    application::SessionPreparationServices services(
        [](const session::Session &value) -> Result<std::vector<tui::Block>> {
            return std::vector<tui::Block>{{.kind = tui::BlockKind::NOTICE,
                                            .state = tui::BlockState::COMPLETED,
                                            .text = "projected " + std::to_string(value.entries.size())}};
        },
        [](const std::optional<session::SessionModelPreference> &) -> Result<application::SessionModelResolution> {
            return application::SessionModelResolution{
                .model = {.entry = {.provider = "test", .id = "test", .name = "Test"}},
            };
        });
    application::SessionCoordinator coordinator(storage->repository, std::move(services));
    auto switching = coordinator.begin_switch(live.id, target_id);
    require(switching && switching->state() == application::SessionSwitchState::PREPARED && live.id == live_id &&
                live.entries.size() == live_entries,
            "preparing a valid switch partially changed the live session");
    switching->flush_current(nullptr);
    require(switching->state() == application::SessionSwitchState::READY && live.id == live_id,
            "switch readiness changed live state before explicit consumption");
    auto &state = *switching;
    auto prepared = std::move(state).take_target();
    require(state.state() == application::SessionSwitchState::CONSUMED && prepared.session.id == target_id &&
                prepared.transcript.size() == 1 && live.id == live_id,
            "prepared switch did not yield one complete target while preserving the old live value");
}

void test_removed_archive_surface_and_state_root_override() {
    TemporaryState temporary;
#ifdef _WIN32
    require(_putenv_s("LIMINAL_STATE_DIR", temporary.root.string().c_str()) == 0, "failed to set state-root override");
#else
    require(setenv("LIMINAL_STATE_DIR", temporary.root.c_str(), 1) == 0, "failed to set state-root override");
#endif
    auto resolved = session::state_root_path();
    require(resolved && *resolved == temporary.root, "LIMINAL_STATE_DIR did not select the state root");
#ifdef _WIN32
    _putenv_s("LIMINAL_STATE_DIR", "");
#else
    unsetenv("LIMINAL_STATE_DIR");
#endif
}

void test_stale_catalog_hint_removal_requires_session_lease() {
    TemporaryState temporary;
    auto storage = open_storage(temporary.root);
    require(storage.has_value(), "failed to open stale-hint lease fixture");
    const auto id = deterministic_id(0x5151);
    require(
        storage->catalog
            .upsert({.summary = {.id = id, .updated_at_ms = 1, .preview = "stale"}, .observed_revision = 1, .workspace_key = "workspace"})
            .has_value(),
        "failed to install stale catalog hint");
    {
        auto lease = session::acquire_session_lease(temporary.root, id);
        require(lease.has_value(), "failed to hold stale-hint session lease");
        auto refused = storage->repository.remove_catalog_hint_if_authority_absent(id);
        auto retained = storage->catalog.find(id);
        require(!refused && retained && *retained, "stale catalog hint was deleted without owning its session lease");
    }
    auto removed = storage->repository.remove_catalog_hint_if_authority_absent(id);
    auto absent = storage->catalog.find(id);
    require(removed && *removed && absent && !*absent, "lease-scoped stale catalog hint removal did not complete");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--lease-child") return lease_child(argv[2], argv[3]);
    const auto executable = std::filesystem::absolute(argv[0]);
    test_topology_publication_and_minimal_schemas();
    test_independent_session_write_transactions();
    test_cross_process_lease_exclusion(executable);
    test_encoding_failure_precedes_begin();
    test_catalog_failure_cannot_rollback_session_and_pending_recovers();
    test_catalog_failure_preserves_exact_authority_and_new_persistence();
    test_blocked_catalog_projection_does_not_block_semantic_commit();
    test_revision_guard_and_precommit_marker();
    test_normal_startup_does_not_scan_sessions();
    test_invalid_pending_marker_is_reported_without_scanning_sessions();
    test_missing_catalog_rebuild_reads_only_singletons();
    test_catalog_ingestion_rejects_invalid_singleton_projections();
    test_missing_catalog_rebuild_includes_leased_sessions();
    test_incomplete_catalog_rebuild_is_resumed_after_crash();
    test_concurrent_catalog_initialization_is_serialized();
    test_empty_catalog_creation_crash_is_recovered_exclusively();
    test_corrupt_catalog_repair_requires_exclusive_maintenance();
    test_missing_catalog_recreation_requires_exclusive_maintenance();
    test_staging_cancellation_and_complete_publication();
    test_publication_retries_across_rename_and_reopen_boundaries();
    test_concurrent_publication_revalidates_materialization_under_lock();
    test_unpublished_queue_recovers_post_rename_attachment();
    test_abandoned_staging_and_marker_are_reconciled();
    test_staged_final_snapshot_is_fully_validated();
    test_recency_rename_fork_and_discovery();
    test_restart_branching_preserves_history_and_cursor();
    test_catalog_keyset_paging_and_index_plan();
    test_recovery_and_transcript_hydration_survive_restart();
    test_persistence_queue_ordering_retry_and_flush_barriers();
    test_reopening_queue_tracks_asynchronous_catalog_recovery();
    test_flush_waits_for_complete_pending_prefix();
    test_initial_publication_serializes_enqueued_mutations();
    test_exact_open_bypasses_catalog_and_failed_hint_preserves_live_session();
    test_prepared_switch_keeps_live_state_atomic();
    test_stale_catalog_hint_removal_requires_session_lease();
    test_removed_archive_surface_and_state_root_override();
    return 0;
}
