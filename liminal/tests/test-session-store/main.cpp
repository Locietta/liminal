#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

#include <liminal/application/session_coordinator.h>
#include <liminal/session/catalog.h>
#include <liminal/session/paths.h>
#include <liminal/session/repository.h>
#include <liminal/session/store.h>

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

struct Storage {
    session::SessionCatalog catalog;
    session::SessionRepository repository;
};

Result<Storage> open_storage(const std::filesystem::path &root) {
    auto catalog = session::SessionCatalog::open(root);
    if (!catalog) return lighter::outcome_error(std::move(catalog).error());
    auto repository = session::SessionRepository::open(root, *catalog);
    if (!repository) return lighter::outcome_error(std::move(repository).error());
    return Storage{.catalog = *std::move(catalog), .repository = *std::move(repository)};
}

session::Session make_session(std::string task, i64 admission_time = 1'000'000) {
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "C:/workspace", .key = "workspace"};
    value.metadata.working_directory = "C:/workspace";
    value.start_task(std::move(task), std::max(admission_time, value.metadata.created_at_ms));
    return value;
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
        require(committed && committed->catalog_degradation, "catalog lock was not reported as non-fatal projection degradation");
        const session::StatePaths paths{temporary.root};
        require(scalar_i64(paths.session_database(id), "SELECT revision FROM session") == 2 &&
                    scalar_i64(paths.session_database(id), "SELECT updated_at_ms FROM session") == expected_recency &&
                    std::filesystem::exists(paths.pending_marker(id)),
                "catalog failure rolled back or lost the authoritative mutation marker");
        execute(catalog_blocker, "ROLLBACK", "failed to unlock catalog");
        sqlite3_close(catalog_blocker);
        published->value.set_model_preference("provider", "model", std::nullopt);
        auto retried = published->writer.commit(session::make_delta(published->value, {}));
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

void test_corrupt_catalog_repair_requires_exclusive_maintenance() {
    TemporaryState temporary;
    session::SessionId id;
    {
        auto storage = open_storage(temporary.root);
        auto published = storage ? publish(storage->repository, make_session("repair catalog")) :
                                   Result<Published>{lighter::outcome_error(storage.error())};
        require(published.has_value(), "failed to publish corrupt-catalog fixture");
        id = published->value.id;
        auto refused = session::SessionCatalog::repair_corrupt(temporary.root);
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
    require(!session::SessionCatalog::open(temporary.root), "corrupt catalog was silently adopted");
    auto repaired_catalog = session::SessionCatalog::repair_corrupt(temporary.root);
    require(repaired_catalog.has_value(), "exclusive corrupt-catalog replacement failed");
    auto repository = session::SessionRepository::open(temporary.root, *repaired_catalog);
    require(repository.has_value(), "repaired catalog could not rebuild from authoritative singletons");
    auto page = repaired_catalog->page({.workspace_key = "workspace"});
    require(page && page->sessions.size() == 1 && page->sessions.front().id == id,
            "corrupt-catalog repair did not rebuild published session discovery");
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
    application::SessionCoordinator coordinator(storage->repository, storage->catalog, std::move(services));
    auto switching = coordinator.begin_switch(live.id, missing);
    require(!switching && live.id == live_id && live.entries.size() == live_entries,
            "failed catalog-hint acquisition mutated the currently active session");
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

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--lease-child") return lease_child(argv[2], argv[3]);
    const auto executable = std::filesystem::absolute(argv[0]);
    test_topology_publication_and_minimal_schemas();
    test_independent_session_write_transactions();
    test_cross_process_lease_exclusion(executable);
    test_encoding_failure_precedes_begin();
    test_catalog_failure_cannot_rollback_session_and_pending_recovers();
    test_revision_guard_and_precommit_marker();
    test_normal_startup_does_not_scan_sessions();
    test_invalid_pending_marker_is_reported_without_scanning_sessions();
    test_missing_catalog_rebuild_reads_only_singletons();
    test_corrupt_catalog_repair_requires_exclusive_maintenance();
    test_staging_cancellation_and_complete_publication();
    test_recency_rename_fork_and_discovery();
    test_exact_open_bypasses_catalog_and_failed_hint_preserves_live_session();
    test_removed_archive_surface_and_state_root_override();
    return 0;
}
