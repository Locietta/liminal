#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <concepts>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>
#include <sqlite3.h>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>

#include <liminal/application/session_coordinator.h>
#include <liminal/agent/agent.h>
#include <liminal/context/context.h>
#include <liminal/model/catalog.h>
#include <liminal/session/codec.h>
#include <liminal/session/persistence.h>
#include <liminal/session/paths.h>
#include <liminal/session/recovery.h>
#include <liminal/session/store.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/hydration.h>
#include <liminal/tui/console_renderer.h>
#include <liminal/tui/selectable_list_dialog.h>
#include <liminal/tui/session_commands.h>

namespace {

using namespace lighter::types;
using namespace liminal;

template <typename T>
concept PubliclyEnqueueable = requires(T &queue, session::SessionDelta delta) { queue.enqueue(std::move(delta)); };

template <typename T>
concept PubliclyDegradable = requires(T &queue) { queue.mark_degraded("failure"); };

template <typename T>
concept LvalueTargetTakeable = requires(T &value) { value.take_target(); };

static_assert(!std::default_initializable<session::Store>);
static_assert(!std::default_initializable<session::SessionWriter>);
static_assert(!std::default_initializable<application::SessionPreparationServices>);
static_assert(!LvalueTargetTakeable<application::SessionSwitch>);
static_assert(std::movable<session::Session>);
static_assert(!std::copy_constructible<session::Session>);
static_assert(!std::assignable_from<session::Session &, const session::Session &>);
static_assert(std::same_as<decltype(std::declval<const session::Session &>().persistence_queue()), const session::PersistenceQueue *>);
static_assert(!PubliclyEnqueueable<session::PersistenceQueue>);
static_assert(!PubliclyDegradable<session::PersistenceQueue>);

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct TemporaryDatabase {
    TemporaryDatabase() {
        directory = std::filesystem::temp_directory_path() / ("liminal-session-test-" + session::to_string(session::generate_session_id()));
        std::filesystem::create_directories(directory);
        path = directory / "state.sqlite3";
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

session::SessionId deterministic_id(u64 value) {
    session::SessionId id;
    for (usize index = 0; index < sizeof(value); ++index) {
        id.bytes[id.bytes.size() - 1 - index] = static_cast<u8>(value >> (index * 8));
    }
    return id;
}

void insert_catalog_rows(const std::filesystem::path &path, u64 first, u64 count, std::string_view workspace, i64 first_timestamp,
                         bool archived = false, i64 timestamp_step = 1) {
    sqlite3 *raw = nullptr;
    require(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK, "failed to open catalog fixture database");
    require(sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK, "failed to begin catalog fixture");
    sqlite3_stmt *insert = nullptr;
    constexpr auto sql = R"sql(
INSERT INTO sessions(id,created_at_ms,updated_at_ms,workspace_root,workspace_key,working_directory,title,preview,
                     archived_at_ms,revision)
VALUES(?1,?2,?3,?4,?4,?4,?5,?6,?7,0)
)sql";
    require(sqlite3_prepare_v2(raw, sql, -1, &insert, nullptr) == SQLITE_OK, "failed to prepare catalog fixture insert");
    for (u64 offset = 0; offset < count; ++offset) {
        const auto id = deterministic_id(first + offset);
        const auto updated = first_timestamp + static_cast<i64>(offset) * timestamp_step;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_blob(insert, 1, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert, 2, updated);
        sqlite3_bind_int64(insert, 3, updated);
        sqlite3_bind_text(insert, 4, workspace.data(), static_cast<int>(workspace.size()), SQLITE_TRANSIENT);
        const auto title = "Session " + std::to_string(first + offset);
        const auto preview = "Preview " + std::to_string(first + offset);
        sqlite3_bind_text(insert, 5, title.data(), static_cast<int>(title.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 6, preview.data(), static_cast<int>(preview.size()), SQLITE_TRANSIENT);
        if (archived)
            sqlite3_bind_int64(insert, 7, updated);
        else
            sqlite3_bind_null(insert, 7);
        require(sqlite3_step(insert) == SQLITE_DONE, "failed to insert catalog fixture row");
    }
    sqlite3_finalize(insert);
    require(sqlite3_exec(raw, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK, "failed to commit catalog fixture");
    sqlite3_close(raw);
}

model::Catalog test_model_catalog() {
    model::Catalog catalog(model::CatalogSources{
        .load = []() -> Result<provider::Registry> {
            provider::Registry registry;
            registry.providers.push_back({
                .id = "test",
                .name = "Test",
                .api = provider::ApiType::OPENAI_RESPONSES,
                .base_url = "https://example.invalid",
                .models = {{.provider = "test", .id = "fallback", .name = "Fallback"}},
            });
            return registry;
        },
        .discover = [](const provider::Registry &, const provider::Instance &)
            -> lighter::Task<std::vector<provider::DiscoveredModel>, Error> { co_return std::vector<provider::DiscoveredModel>{}; },
    });
    lighter::EventLoop loop;
    auto refreshed = catalog.refresh();
    loop.schedule(refreshed);
    loop.run();
    require(refreshed.result().has_value(), "failed to refresh test model catalog");
    return catalog;
}

model::Catalog changing_model_catalog(const std::shared_ptr<std::string> &model_id) {
    return model::Catalog(model::CatalogSources{
        .load = [model_id]() -> Result<provider::Registry> {
            provider::Registry registry;
            registry.providers.push_back({
                .id = "test",
                .name = "Test",
                .api = provider::ApiType::OPENAI_RESPONSES,
                .base_url = "https://example.invalid",
                .models = {{.provider = "test", .id = *model_id, .name = *model_id}},
            });
            return registry;
        },
        .discover = [](const provider::Registry &, const provider::Instance &)
            -> lighter::Task<std::vector<provider::DiscoveredModel>, Error> { co_return std::vector<provider::DiscoveredModel>{}; },
    });
}

void refresh_catalog(model::Catalog &catalog) {
    lighter::EventLoop loop;
    auto refreshed = catalog.refresh();
    loop.schedule(refreshed);
    loop.run();
    require(refreshed.result().has_value(), "failed to refresh model catalog fixture");
}

application::SessionPreparationServices preparation_services(model::Catalog &models) {
    return application::SessionPreparationServices(
        [](const session::Session &value) -> Result<std::vector<tui::Block>> {
            return std::vector<tui::Block>{{.kind = tui::BlockKind::NOTICE,
                                            .state = tui::BlockState::COMPLETED,
                                            .text = "projected " + std::to_string(value.entries.size())}};
        },
        [&models](const std::optional<session::SessionModelPreference> &stored_model) {
            return application::resolve_session_model(models, std::nullopt, stored_model);
        });
}

void persist_session(session::Store &store, const session::Session &value) {
    auto writer = store.lease(value.id);
    require(writer && writer->commit(session::make_delta(value, value.entries)), "failed to persist session fixture");
}

void set_environment(std::string_view name, const std::optional<std::string> &value) {
#ifdef _WIN32
    require(_putenv_s(std::string(name).c_str(), value ? value->c_str() : "") == 0, "failed to update test environment");
#else
    const auto result = value ? setenv(std::string(name).c_str(), value->c_str(), 1) : unsetenv(std::string(name).c_str());
    require(result == 0, "failed to update test environment");
#endif
}

struct EnvironmentRestore {
    explicit EnvironmentRestore(std::string name) : name(std::move(name)) {
        if (const auto *value = std::getenv(this->name.c_str())) original = value;
    }
    ~EnvironmentRestore() { set_environment(name, original); }

    std::string name;
    std::optional<std::string> original;
};

provider::ToolCall tool_call(std::string id, std::string name = "read_file") {
    glz::generic input;
    const auto error = glz::read_json(input, R"({"path":"README.md"})");
    require(!error, "failed to create tool input fixture");
    return {.id = std::move(id), .name = std::move(name), .input = std::move(input)};
}

session::Session exhaustive_session() {
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "D:/work/project", .key = "d:/work/project"};
    value.metadata.working_directory = "D:/work/project/subdir";
    value.metadata.title = "Durable session";
    value.metadata.model_preference =
        session::SessionModelPreference{.provider = "openai", .model = "gpt-test", .reasoning_effort = "high"};
    value.metadata.forked_from = session::ForkOrigin{.session = session::generate_session_id(), .entry = session::EntryId{.value = 9}};

    const auto task = value.start_task("persist everything");
    const auto call = value.next_provider_call();
    for (const auto phase : {provider::MessagePhase::UNSPECIFIED, provider::MessagePhase::COMMENTARY, provider::MessagePhase::FINAL}) {
        value.append(session::OutputItemCompleted{
            .task_id = task,
            .provider_call_id = call,
            .item =
                provider::AssistantMessageItem{
                    .id = {.value = "message-" + std::to_string(static_cast<int>(phase))},
                    .parts = {{.text = "message text"}},
                    .phase = phase,
                },
        });
    }
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "tool-item"}, .call = tool_call("call-1")},
    });
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item =
            provider::ProviderOpaqueItem{
                .id = {.value = "opaque-item"},
                .part = {.provider_tag = "openai", .payload = R"({"encrypted":"bytes"})"},
            },
    });

    const std::vector stops{provider::StopKind::DONE,    provider::StopKind::NEEDS_TOOL_RESULTS, provider::StopKind::TRUNCATED,
                            provider::StopKind::REFUSED, provider::StopKind::CONTEXT_EXHAUSTED,  provider::StopKind::OTHER};
    for (usize index = 0; index < stops.size(); ++index) {
        const auto loop = index % 3 == 0 ? session::ProviderCallLoopOutcome::FOLLOW_UP :
                          index % 3 == 1 ? session::ProviderCallLoopOutcome::TERMINAL :
                                           session::ProviderCallLoopOutcome::FAILED;
        value.append(session::ProviderCallCompleted{
            .task_id = task,
            .id = {.value = static_cast<u64>(index + 1)},
            .completion =
                {
                    .stop = stops[index],
                    .stop_detail = "native stop",
                    .usage = {.input_tokens = 10,
                              .output_tokens = 5,
                              .cache_read_tokens = 2,
                              .cache_write_tokens = 1,
                              .reasoning_tokens = 3,
                              .context_tokens = 15},
                    .model = "provider-model",
                    .request_id = "request-id",
                },
            .loop_outcome = loop,
        });
    }
    for (const auto reason : {session::ProviderCallAbortReason::CANCELLED, session::ProviderCallAbortReason::FAILED,
                              session::ProviderCallAbortReason::INTERRUPTED}) {
        value.append(session::ProviderCallAborted{
            .task_id = task,
            .id = {.value = value.next_provider_call_id++},
            .reason = reason,
            .detail = "bounded diagnostic",
        });
    }
    value.append(session::ToolResults{
        .task_id = task,
        .provider_call_id = call,
        .results = {{.call_id = "call-1", .content = "file contents"},
                    {.call_id = "call-2", .content = "outcome unknown", .is_error = true}},
    });
    for (const auto outcome : {session::TaskOutcome::COMPLETED, session::TaskOutcome::CANCELLED, session::TaskOutcome::FAILED,
                               session::TaskOutcome::INTERRUPTED}) {
        value.append(session::TaskFinished{.id = task, .outcome = outcome});
    }

    session::ContextCheckpoint checkpoint;
    checkpoint.items.push_back(session::ContextInput{.parts = {
                                                         provider::TextPart{.text = "summary"},
                                                         tool_call("checkpoint-call"),
                                                         provider::ToolResult{.call_id = "checkpoint-call", .content = "result"},
                                                         provider::OpaquePart{.provider_tag = "openai", .payload = "opaque"},
                                                     }});
    checkpoint.items.push_back(session::CheckpointOutput{.item = provider::AssistantMessageItem{
                                                             .id = {.value = "checkpoint-message"},
                                                             .parts = {{.text = "answer"}},
                                                             .phase = provider::MessagePhase::FINAL,
                                                         }});
    checkpoint.items.push_back(session::CheckpointOutput{
        .item = provider::ToolCallItem{.id = {.value = "checkpoint-tool"}, .call = tool_call("checkpoint-tool-call")},
    });
    checkpoint.items.push_back(session::CheckpointOutput{.item = provider::ProviderOpaqueItem{
                                                             .id = {.value = "checkpoint-opaque"},
                                                             .part = {.provider_tag = "openai", .payload = "private"},
                                                         }});
    value.append(std::move(checkpoint));
    value.next_provider_call_id = 20;
    return value;
}

void require_payloads_equal(const session::Session &left, const session::Session &right) {
    require(left.entries.size() == right.entries.size(), "round trip changed the entry count");
    for (usize index = 0; index < left.entries.size(); ++index) {
        const auto encoded_left = session::encode_payload(left.entries[index].payload);
        const auto encoded_right = session::encode_payload(right.entries[index].payload);
        require(encoded_left && encoded_right, "round-trip payload could not be encoded");
        require(encoded_left->kind == encoded_right->kind && encoded_left->version == encoded_right->version &&
                    encoded_left->json == encoded_right->json,
                "round trip changed a semantic payload");
        require(left.entries[index].parent_id == right.entries[index].parent_id &&
                    left.entries[index].created_at_ms == right.entries[index].created_at_ms,
                "round trip changed entry envelope data");
    }
}

void test_uuid_v7() {
    const auto id = session::generate_session_id();
    require((id.bytes[6] >> 4) == 7, "generated session id is not UUIDv7");
    require((id.bytes[8] & 0xc0) == 0x80, "generated session id has the wrong RFC variant");
    const auto rendered = session::to_string(id);
    const auto parsed = session::parse_session_id(rendered);
    require(parsed && *parsed == id && rendered.size() == 36, "session id text did not round trip");
}

void test_store_preserves_caller_owned_directory_permissions() {
#ifndef _WIN32
    TemporaryDatabase database;
    constexpr auto requested = std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                               std::filesystem::perms::others_read | std::filesystem::perms::others_exec;
    std::filesystem::permissions(database.directory, requested, std::filesystem::perm_options::replace);
    const auto before = std::filesystem::status(database.directory).permissions();
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open store in caller-owned directory");
    const auto after = std::filesystem::status(database.directory).permissions();
    require(after == before, "Store::open changed permissions on a caller-owned directory");
    constexpr auto public_permissions = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    require((std::filesystem::status(database.path).permissions() & public_permissions) == std::filesystem::perms::none,
            "new state database was visible outside its owner");

    session::Session value;
    value.metadata.working_directory = database.directory.generic_string();
    value.start_task("create WAL sidecar");
    auto writer = store->lease(value.id);
    require(writer && writer->commit(session::make_delta(value, value.entries)), "failed to create WAL sidecar fixture");
    const auto wal = std::filesystem::path(database.path.string() + "-wal");
    require(std::filesystem::exists(wal) &&
                (std::filesystem::status(wal).permissions() & public_permissions) == std::filesystem::perms::none,
            "SQLite WAL sidecar was created with public permissions");
#endif
}

void test_established_database_requires_application_identity() {
    TemporaryDatabase database;
    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to create foreign database fixture");
    require(sqlite3_exec(raw, "PRAGMA user_version=1", nullptr, nullptr, nullptr) == SQLITE_OK,
            "failed to set foreign database schema version");
    sqlite3_close(raw);

    auto opened = session::Store::open(database.path);
    require(!opened && opened.error().detail.contains("application identity"),
            "an established database without Liminal's application identity was accepted");

    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to reopen rejected database fixture");
    sqlite3_stmt *journal = nullptr;
    require(sqlite3_prepare_v2(raw, "PRAGMA journal_mode", -1, &journal, nullptr) == SQLITE_OK && sqlite3_step(journal) == SQLITE_ROW,
            "failed to inspect rejected database journal mode");
    const auto mode = std::string(reinterpret_cast<const char *>(sqlite3_column_text(journal, 0)));
    sqlite3_finalize(journal);
    sqlite3_close(raw);
    require(mode != "wal", "Store::open changed a rejected foreign database to WAL mode");
}

void test_unidentified_nonempty_database_is_not_adopted() {
    TemporaryDatabase database;
    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to create unidentified database fixture");
    require(sqlite3_exec(raw, "CREATE TABLE sqliteX(value TEXT)", nullptr, nullptr, nullptr) == SQLITE_OK,
            "failed to create foreign table fixture");
    sqlite3_close(raw);

    auto opened = session::Store::open(database.path);
    require(!opened && opened.error().detail.contains("unidentified non-empty"), "an unidentified non-empty database was adopted");

    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to reopen unidentified database fixture");
    sqlite3_stmt *inspection = nullptr;
    constexpr auto query = R"sql(
SELECT
    (SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sqliteX'),
    (SELECT application_id FROM pragma_application_id),
    (SELECT user_version FROM pragma_user_version),
    (SELECT journal_mode FROM pragma_journal_mode)
)sql";
    require(sqlite3_prepare_v2(raw, query, -1, &inspection, nullptr) == SQLITE_OK && sqlite3_step(inspection) == SQLITE_ROW,
            "failed to inspect rejected unidentified database");
    require(sqlite3_column_int(inspection, 0) == 1 && sqlite3_column_int(inspection, 1) == 0 && sqlite3_column_int(inspection, 2) == 0 &&
                std::string(reinterpret_cast<const char *>(sqlite3_column_text(inspection, 3))) != "wal",
            "Store::open mutated a rejected unidentified database");
    sqlite3_finalize(inspection);
    sqlite3_close(raw);
}

void test_catalog_index_migrates_from_phase_one_schema() {
    TemporaryDatabase database;
    {
        auto created = session::Store::open(database.path);
        require(created.has_value(), "failed to create migration fixture");
    }
    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to open migration fixture");
    require(sqlite3_exec(raw, "DROP INDEX sessions_workspace_archived_recent; PRAGMA user_version=1", nullptr, nullptr, nullptr) ==
                SQLITE_OK,
            "failed to restore phase-one schema fixture");
    sqlite3_close(raw);

    auto migrated = session::Store::open(database.path);
    require(migrated.has_value(), "failed to migrate phase-one session schema");
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to inspect migrated schema");
    sqlite3_stmt *inspection = nullptr;
    constexpr auto query = R"sql(
SELECT
    (SELECT user_version FROM pragma_user_version),
    (SELECT count(*) FROM sqlite_schema WHERE type='index' AND name='sessions_workspace_archived_recent')
)sql";
    require(sqlite3_prepare_v2(raw, query, -1, &inspection, nullptr) == SQLITE_OK && sqlite3_step(inspection) == SQLITE_ROW &&
                sqlite3_column_int(inspection, 0) == 2 && sqlite3_column_int(inspection, 1) == 1,
            "phase-one migration did not install the archived-session index exactly once");
    sqlite3_finalize(inspection);
    sqlite3_close(raw);
}

void test_writer_rejects_updated_at_regression() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open timestamp test store");
    session::Session value;
    value.metadata.working_directory = database.directory.generic_string();
    value.start_task("timestamp test");
    value.metadata.updated_at_ms = value.metadata.created_at_ms + 100;
    auto writer = store->lease(value.id);
    require(writer && writer->commit(session::make_delta(value, value.entries)), "failed to seed timestamp test session");

    value.metadata.updated_at_ms -= 50;
    auto regressed = writer->commit(session::make_delta(value, {}));
    require(!regressed && regressed.error().detail.contains("timestamps"), "writer accepted a regressing durable update timestamp");
}

void test_windows_workspace_key_uses_unicode_case_mapping() {
#ifdef _WIN32
    TemporaryDatabase database;
    const auto upper = database.directory / std::filesystem::path(L"\u00c4rea");
    std::filesystem::create_directory(upper);
    const auto lower = database.directory / std::filesystem::path(L"\u00e4REA");
    const auto upper_identity = session::workspace_identity(upper);
    const auto lower_identity = session::workspace_identity(lower);
    require(upper_identity && lower_identity && upper_identity->key == lower_identity->key,
            "Windows workspace key did not apply native Unicode case mapping");
#endif
}

void test_store_round_trip_and_restart_branching() {
    TemporaryDatabase database;
    auto opened = session::Store::open(database.path);
    if (!opened) throw std::runtime_error(opened.error().message());
    auto store = *std::move(opened);
    const auto missing_latest = store.latest("missing-workspace");
    const auto missing_id = store.resolve_id(session::to_string(session::generate_session_id()));
    require(!missing_latest && missing_latest.error().detail == "no saved session exists in this workspace" && !missing_id &&
                missing_id.error().detail == "session was not found",
            "absent session lookups did not return domain errors");
    auto original = exhaustive_session();
    auto writer = store.lease(original.id);
    require(writer.has_value(), "failed to acquire the first session writer");
    auto conflicting = store.lease(original.id);
    require(!conflicting && conflicting.error().detail.contains("in use"), "a second session lease was accepted");

    auto invalid_delta = session::make_delta(original, original.entries);
    invalid_delta.active_leaf = session::EntryId{.value = original.entries.size() + 1};
    require(!writer->commit(invalid_delta), "writer persisted a delta with an unknown active leaf");
    auto committed = writer->commit(session::make_delta(original, original.entries));
    require(committed.has_value(), "initial session commit failed");
    auto latest = store.latest(original.metadata.workspace->key);
    require(latest && latest->id == original.id && latest->entry_count == original.entries.size(),
            "indexed latest-session lookup returned the wrong catalog row");
    const auto full = store.resolve_id(session::to_string(original.id));
    const auto prefix = store.resolve_id(session::to_string(original.id).substr(0, 12));
    require(full && *full == original.id && prefix && *prefix == original.id, "direct or prefix session lookup failed");

    auto sibling_id = original.id;
    sibling_id.bytes.back() ^= 1;
    session::Session sibling(sibling_id);
    sibling.metadata.working_directory = database.directory.generic_string();
    sibling.start_task("prefix sibling");
    auto sibling_writer = store.lease(sibling.id);
    require(sibling_writer && sibling_writer->commit(session::make_delta(sibling, sibling.entries)),
            "failed to seed ambiguous prefix fixture");
    const auto ambiguous = store.resolve_id(session::to_string(original.id).substr(0, 8));
    require(!ambiguous && ambiguous.error().detail.contains("ambiguous"), "indexed prefix lookup did not detect ambiguity");

    sqlite3 *query_plan = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &query_plan) == SQLITE_OK, "failed to inspect prefix query plan");
    sqlite3_stmt *plan = nullptr;
    require(sqlite3_prepare_v2(query_plan, "EXPLAIN QUERY PLAN SELECT id FROM sessions WHERE id >= ?1 AND id < ?2 ORDER BY id LIMIT 2", -1,
                               &plan, nullptr) == SQLITE_OK,
            "failed to prepare prefix query plan");
    sqlite3_bind_blob(plan, 1, original.id.bytes.data(), static_cast<int>(original.id.bytes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(plan, 2, sibling_id.bytes.data(), static_cast<int>(sibling_id.bytes.size()), SQLITE_TRANSIENT);
    require(sqlite3_step(plan) == SQLITE_ROW, "prefix query plan was empty");
    const auto plan_detail = std::string(reinterpret_cast<const char *>(sqlite3_column_text(plan, 3)));
    require(plan_detail.contains("SEARCH") && !plan_detail.contains("SCAN"), "prefix lookup query does not use the session id index");
    sqlite3_finalize(plan);
    sqlite3_close(query_plan);

    auto loaded = writer->load();
    require(loaded.has_value(), "failed to reload committed session");
    require_payloads_equal(original, *loaded);
    require(loaded->metadata.title == original.metadata.title &&
                loaded->metadata.model_preference->reasoning_effort == std::optional<std::string>{"high"} &&
                loaded->active_leaf == original.active_leaf,
            "round trip changed session catalog metadata");

    const auto original_leaf = *loaded->active_leaf;
    auto selected = loaded->select_leaf(session::EntryId{.value = 1});
    require(selected.has_value(), "failed to rewind loaded session");
    auto cursor_commit = writer->commit(session::make_delta(*loaded, {}));
    require(cursor_commit.has_value(), "cursor-only mutation did not persist");
    loaded->start_task("alternate branch");
    const auto alternate = loaded->entries.back();
    auto branch_commit = writer->commit(session::make_delta(*loaded, std::span(&alternate, 1)));
    require(branch_commit.has_value(), "branch append did not persist");
    auto restarted = writer->load();
    require(restarted && restarted->find(original_leaf) && restarted->entries.back().parent_id == session::EntryId{.value = 1},
            "restart branching deleted or rewrote the original descendant");
}

void test_unknown_payload_version_isolated_from_catalog() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open version test store");
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    value.metadata.working_directory = database.directory.generic_string();
    value.start_task("version test");
    auto writer = store->lease(value.id);
    require(writer.has_value(), "failed to lease version test session");
    require(writer->commit(session::make_delta(value, value.entries)).has_value(), "failed to seed version test session");

    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to open raw SQLite test connection");
    require(sqlite3_exec(raw, "UPDATE session_entries SET payload_version=99", nullptr, nullptr, nullptr) == SQLITE_OK,
            "failed to corrupt payload version fixture");
    sqlite3_close(raw);

    auto loaded = writer->load();
    require(!loaded && loaded.error().detail.contains("unsupported TaskStarted payload version 99"),
            "unknown payload version did not fail with a precise error");
    auto latest = store->latest("workspace");
    auto page = store->page({.workspace_key = "workspace"});
    require(latest && latest->id == value.id && page && page->sessions.size() == 1 && page->sessions.front().id == value.id,
            "unknown entry payload version made the catalog unlistable");
}

void test_catalog_keyset_paging() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open catalog paging store");
    insert_catalog_rows(database.path, 1, 1, "workspace-a", 100);
    insert_catalog_rows(database.path, 2, 2, "workspace-a", 200, false, 0);
    insert_catalog_rows(database.path, 4, 3, "workspace-a", 300, false, 100);
    insert_catalog_rows(database.path, 100, 1, "workspace-a", 1'000, true);
    insert_catalog_rows(database.path, 200, 1, "workspace-b", 2'000);

    auto first = store->page({.workspace_key = "workspace-a", .limit = 2});
    require(first && first->sessions.size() == 2 && first->sessions[0].id == deterministic_id(6) &&
                first->sessions[1].id == deterministic_id(5) && first->continuation,
            "first catalog page was not newest-first or did not expose a continuation");
    auto middle = store->page({.workspace_key = "workspace-a", .after = first->continuation, .limit = 2});
    require(middle && middle->sessions.size() == 2 && middle->sessions[0].id == deterministic_id(4) &&
                middle->sessions[1].id == deterministic_id(3) && middle->continuation,
            "middle catalog page did not retain stable tied-timestamp ordering");
    auto final = store->page({.workspace_key = "workspace-a", .after = middle->continuation, .limit = 2});
    require(final && final->sessions.size() == 2 && final->sessions[0].id == deterministic_id(2) &&
                final->sessions[1].id == deterministic_id(1) && !final->continuation,
            "final catalog page was incorrect or exposed a false continuation");
    auto beyond_final = store->page(
        {.workspace_key = "workspace-a", .after = session::SessionPageCursor{.updated_at_ms = 100, .id = deterministic_id(1)}, .limit = 2});
    auto empty = store->page({.workspace_key = "empty-workspace", .limit = 2});
    require(beyond_final && beyond_final->sessions.empty() && !beyond_final->continuation && empty && empty->sessions.empty() &&
                !empty->continuation,
            "empty catalog pages were not successful terminal pages");

    auto archived = store->page({.workspace_key = "workspace-a", .state = session::SessionCatalogState::ARCHIVED, .limit = 50});
    require(archived && archived->sessions.size() == 1 && archived->sessions.front().id == deterministic_id(100),
            "archived catalog did not isolate archived sessions");
    auto other_workspace = store->page({.workspace_key = "workspace-b", .limit = 50});
    require(other_workspace && other_workspace->sessions.size() == 1 && other_workspace->sessions.front().id == deterministic_id(200),
            "catalog paging crossed canonical workspace boundaries");

    insert_catalog_rows(database.path, 1'000, 60, "page-cap", 1'000);
    auto capped = store->page({.workspace_key = "page-cap", .limit = 100});
    require(capped && capped->sessions.size() == 50 && capped->continuation, "catalog query did not enforce its 50-row maximum page size");
}

void test_large_catalog_uses_indexed_keyset_query() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open large catalog store");
    insert_catalog_rows(database.path, 10'000, 10'000, "large-workspace", 10'000);

    usize total = 0;
    std::optional<session::SessionPageCursor> cursor;
    do {
        auto page = store->page({.workspace_key = "large-workspace", .after = cursor, .limit = 50});
        require(page.has_value(), "large catalog keyset page failed");
        if (total == 0) {
            require(page->sessions.size() == 50 && page->sessions.front().id == deterministic_id(19'999),
                    "large catalog first page was incorrect");
        }
        total += page->sessions.size();
        cursor = page->continuation;
    } while (cursor);
    require(total == 10'000, "deep keyset traversal skipped or duplicated large-catalog rows");

    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to inspect catalog query plan");
    sqlite3_stmt *plan = nullptr;
    constexpr auto query = R"sql(
EXPLAIN QUERY PLAN
SELECT id, updated_at_ms FROM sessions
WHERE workspace_key=?1 AND archived_at_ms IS NULL
  AND (updated_at_ms, id) < (?2, ?3)
ORDER BY updated_at_ms DESC, id DESC LIMIT ?4
)sql";
    require(sqlite3_prepare_v2(raw, query, -1, &plan, nullptr) == SQLITE_OK, "failed to prepare catalog query plan");
    sqlite3_bind_text(plan, 1, "large-workspace", -1, SQLITE_STATIC);
    sqlite3_bind_int64(plan, 2, 15'000);
    const auto id = deterministic_id(15'000);
    sqlite3_bind_blob(plan, 3, id.bytes.data(), static_cast<int>(id.bytes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(plan, 4, 51);
    std::string details;
    while (sqlite3_step(plan) == SQLITE_ROW) {
        details += reinterpret_cast<const char *>(sqlite3_column_text(plan, 3));
        details += '\n';
    }
    sqlite3_finalize(plan);
    sqlite3_close(raw);
    require(details.contains("sessions_workspace_recent") && !details.contains("session_entries") && !details.contains("USE TEMP B-TREE"),
            "catalog query plan did not use the workspace/recent index directly");
}

void test_session_catalog_mutations_persist_and_respect_ownership() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open catalog mutation store");
    session::Session value;
    value.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    value.metadata.working_directory = "workspace";
    value.start_task("catalog mutation");
    persist_session(*store, value);

    auto models = test_model_catalog();
    application::SessionCoordinator coordinator(*store, preparation_services(models));
    require(coordinator.mutate_inactive(value.id, application::RenameSession{.title = "Named session"}).has_value(),
            "inactive session rename failed");
    require(coordinator.mutate_inactive(value.id, application::ArchiveSession{}).has_value(), "inactive session archive failed");
    auto active = coordinator.page({.workspace_key = "workspace"});
    auto archived = coordinator.page({.workspace_key = "workspace", .state = session::SessionCatalogState::ARCHIVED});
    require(active && active->sessions.empty() && archived && archived->sessions.size() == 1 &&
                archived->sessions.front().title == "Named session",
            "archive mutation did not update catalog visibility immediately");

    {
        auto locked = store->lease(value.id);
        require(locked.has_value(), "failed to hold inactive-session ownership fixture");
        auto rejected = coordinator.mutate_inactive(value.id, application::UnarchiveSession{});
        require(!rejected && rejected.error().detail.contains("in use"), "inactive catalog mutation ignored exclusive session ownership");
    }
    require(coordinator.mutate_inactive(value.id, application::UnarchiveSession{}).has_value(), "inactive session unarchive failed");

    auto restarted = session::Store::open(database.path);
    require(restarted.has_value(), "failed to reopen catalog mutation store");
    auto visible = restarted->page({.workspace_key = "workspace"});
    require(visible && visible->sessions.size() == 1 && visible->sessions.front().title == "Named session",
            "name or unarchive state did not survive restart");
    auto writer = restarted->lease(value.id);
    require(writer.has_value(), "failed to lease restarted catalog mutation session");
    auto loaded = writer->load();
    require(loaded && loaded->metadata.title == "Named session" && !loaded->metadata.archived_at_ms,
            "restart did not hydrate persisted title and archive state");
}

void test_durable_acquisition_precedes_live_model_resolution() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open staged preparation store");

    session::Session target;
    target.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    target.metadata.working_directory = "workspace";
    target.metadata.model_preference = session::SessionModelPreference{.provider = "missing", .model = "removed"};
    target.start_task("staged preparation");
    persist_session(*store, target);

    auto current_model = std::make_shared<std::string>("first");
    auto models = changing_model_catalog(current_model);
    refresh_catalog(models);
    usize resolutions = 0;
    application::SessionPreparationServices services(
        [](const session::Session &) -> Result<std::vector<tui::Block>> {
            return std::vector<tui::Block>{{.kind = tui::BlockKind::USER, .text = "acquired transcript"}};
        },
        [&models, &resolutions](const std::optional<session::SessionModelPreference> &stored_model) {
            ++resolutions;
            return application::resolve_session_model(models, std::nullopt, stored_model);
        });
    application::SessionCoordinator coordinator(*store, std::move(services));

    auto acquired = coordinator.acquire(target.id);
    require(acquired && resolutions == 0, "durable acquisition consulted model policy before discovery completed");
    auto conflicting_lease = store->lease(target.id);
    require(!conflicting_lease && conflicting_lease.error().detail.contains("in use"),
            "durable acquisition did not retain exclusive ownership before model discovery");

    *current_model = "second";
    refresh_catalog(models);
    auto prepared = coordinator.resolve_model(*std::move(acquired));
    require(prepared && resolutions == 1 && prepared->model.entry.id == "second" &&
                std::ranges::any_of(prepared->notices, [](const std::string &notice) { return notice.contains("using test/second"); }),
            "session completion did not resolve fallback policy from the refreshed catalog");
}

void test_prepared_switch_is_atomic_and_explicit_about_unsaved_history() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open switch coordination store");
    auto models = test_model_catalog();

    session::Session target;
    target.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    target.metadata.working_directory = "workspace";
    target.metadata.model_preference = session::SessionModelPreference{.provider = "missing", .model = "removed"};
    const auto target_task = target.start_task("unfinished target");
    const auto target_call = target.next_provider_call();
    target.append(session::OutputItemCompleted{
        .task_id = target_task,
        .provider_call_id = target_call,
        .item =
            provider::ProviderOpaqueItem{.id = {.value = "private"}, .part = {.provider_tag = "missing", .payload = "provider-private"}},
    });
    persist_session(*store, target);

    session::Session recovery_degraded;
    recovery_degraded.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    recovery_degraded.metadata.working_directory = "workspace";
    recovery_degraded.start_task("recovery cannot save");
    persist_session(*store, recovery_degraded);

    session::Session live;
    live.start_task("live session remains authoritative");
    const auto live_entries = live.entries.size();
    const auto live_leaf = live.active_leaf;
    application::SessionCoordinator coordinator(*store, preparation_services(models));

    auto same = coordinator.begin_switch(live.id, live.id);
    require(same && same->state() == application::SessionSwitchState::CURRENT_SELECTED,
            "selecting the current session was not a no-op result");

    {
        auto locked = store->lease(target.id);
        require(locked.has_value(), "failed to hold target lease fixture");
        auto rejected = coordinator.begin_switch(live.id, target.id);
        require(!rejected && rejected.error().detail.contains("in use") && live.entries.size() == live_entries &&
                    live.active_leaf == live_leaf,
                "target lease failure disturbed the live session");
    }

    application::SessionPreparationServices failing_projection = preparation_services(models);
    failing_projection.project = [](const session::Session &) -> Result<std::vector<tui::Block>> {
        return lighter::outcome_error(Error::protocol("injected projection failure"));
    };
    application::SessionCoordinator projection_coordinator(*store, std::move(failing_projection));
    auto projection_failure = projection_coordinator.begin_switch(live.id, target.id);
    require(!projection_failure && projection_failure.error().detail.contains("projection") && live.entries.size() == live_entries,
            "projection failure disturbed the live session");

    auto invalid_model_services = preparation_services(models);
    invalid_model_services.resolve_model = [&models](const std::optional<session::SessionModelPreference> &stored_model) {
        return application::resolve_session_model(models, std::optional<std::string>{"test/does-not-exist"}, stored_model);
    };
    application::SessionCoordinator invalid_model_coordinator(*store, std::move(invalid_model_services));
    auto model_failure = invalid_model_coordinator.begin_switch(live.id, target.id);
    require(!model_failure && model_failure.error().detail.contains("unknown model") && live.entries.size() == live_entries,
            "model-selection failure disturbed the live session");

    {
        auto degraded_services = preparation_services(models);
        degraded_services.persistence = [](session::SessionWriter writer) {
            const auto id = writer.session_id();
            auto lease_owner = std::make_shared<session::SessionWriter>(std::move(writer));
            return session::PersistenceQueue::create_for_test(id, [lease_owner](const session::SessionDelta &) -> Result<void> {
                return lighter::outcome_error(Error::storage("injected recovery save failure"));
            });
        };
        application::SessionCoordinator degraded_coordinator(*store, std::move(degraded_services));
        auto degraded_switch = degraded_coordinator.begin_switch(live.id, recovery_degraded.id);
        require(degraded_switch && degraded_switch->state() == application::SessionSwitchState::PREPARED,
                "target recovery persistence degradation incorrectly rejected the prepared target");
        degraded_switch->flush_current(nullptr);
        auto &switch_state = *degraded_switch;
        auto degraded_target = std::move(switch_state).take_target();
        require(switch_state.state() == application::SessionSwitchState::CONSUMED,
                "taking a prepared target did not consume its switch state");
        const auto degraded_status = degraded_target.session.persistence_queue()->status();
        require(degraded_status.degraded && degraded_status.pending_mutations != 0 &&
                    std::ranges::any_of(degraded_target.notices, [](const std::string &notice) { return notice.contains("not saving"); }) &&
                    live.entries.size() == live_entries,
                "target recovery persistence degradation was not exposed as an unsaved prepared session");
    }

    session::Session corrupt;
    corrupt.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    corrupt.metadata.working_directory = "workspace";
    corrupt.start_task("corrupt target");
    persist_session(*store, corrupt);
    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to open corrupt switch fixture");
    sqlite3_stmt *corrupt_payload = nullptr;
    require(sqlite3_prepare_v2(raw, "UPDATE session_entries SET payload_version=99 WHERE session_id=?1", -1, &corrupt_payload, nullptr) ==
                SQLITE_OK,
            "failed to prepare corrupt switch fixture");
    sqlite3_bind_blob(corrupt_payload, 1, corrupt.id.bytes.data(), static_cast<int>(corrupt.id.bytes.size()), SQLITE_TRANSIENT);
    require(sqlite3_step(corrupt_payload) == SQLITE_DONE, "failed to corrupt switch payload");
    sqlite3_finalize(corrupt_payload);
    sqlite3_close(raw);
    auto load_failure = coordinator.begin_switch(live.id, corrupt.id);
    require(!load_failure && load_failure.error().detail.contains("unsupported") && live.entries.size() == live_entries,
            "target load failure disturbed the live session");

    {
        auto switching = coordinator.begin_switch(live.id, target.id);
        require(switching && switching->state() == application::SessionSwitchState::PREPARED,
                "valid target did not reach the prepared switch state");
        switching->flush_current(nullptr);
        require(switching->state() == application::SessionSwitchState::READY, "a session without queued persistence did not become ready");
        auto &switch_state = *switching;
        auto prepared = std::move(switch_state).take_target();
        require(switch_state.state() == application::SessionSwitchState::CONSUMED,
                "successful target extraction did not become single-use");
        require(prepared.session.entries.size() > target.entries.size() && !prepared.notices.empty() &&
                    prepared.notices.back().contains("stored model") && prepared.model.entry.id == "fallback" &&
                    std::holds_alternative<provider::ProviderOpaqueItem>(
                        std::get<session::OutputItemCompleted>(prepared.session.entries[1].payload).item),
                "target preparation did not recover, fall back, and preserve provider-private history");
    }

    std::atomic<bool> writes_succeed = false;
    session::Session unsaved;
    auto failing_queue =
        session::PersistenceQueue::create_for_test(unsaved.id, [&writes_succeed](const session::SessionDelta &) -> Result<void> {
            if (writes_succeed.load()) return {};
            return lighter::outcome_error(Error::storage("injected unsaved tail"));
        });
    require(unsaved.attach_persistence(failing_queue).has_value(), "failed to attach unsaved-tail fixture");
    unsaved.start_task("unsaved current history");

    {
        auto switching = coordinator.begin_switch(unsaved.id, target.id);
        require(switching.has_value(), "failed to prepare target for decline branch");
        switching->flush_current(failing_queue.get());
        require(switching->state() == application::SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION,
                "failed flush did not require explicit confirmation");
        switching->resolve_unsaved(application::UnsavedSwitchDecision::STAY, failing_queue->status());
        require(switching->state() == application::SessionSwitchState::CANCELLED && unsaved.entries.size() == 1,
                "declining unsaved-tail abandonment changed the live session");
    }
    {
        auto switching = coordinator.begin_switch(unsaved.id, target.id);
        require(switching.has_value(), "failed to prepare target for abandonment branch");
        switching->flush_current(failing_queue.get());
        switching->resolve_unsaved(application::UnsavedSwitchDecision::ABANDON_UNSAVED_HISTORY, failing_queue->status());
        require(switching->state() == application::SessionSwitchState::READY && switching->abandoned_unsaved_history(),
                "accepting confirmation did not explicitly mark old unsaved history abandoned");
    }
    {
        auto switching = coordinator.begin_switch(unsaved.id, target.id);
        require(switching.has_value(), "failed to prepare target for retry branch");
        switching->flush_current(failing_queue.get());
        require(switching->state() == application::SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION,
                "retry branch did not enter confirmation state");
        writes_succeed = true;
        require(failing_queue->flush().has_value(), "observable background retry fixture did not save the pending tail");
        switching->resolve_unsaved(application::UnsavedSwitchDecision::ABANDON_UNSAVED_HISTORY, failing_queue->status());
        require(switching->state() == application::SessionSwitchState::READY && !switching->abandoned_unsaved_history(),
                "a tail saved during confirmation was still classified as abandoned");
    }
}

lighter::Task<lighter::Error> drive_session_picker(tui::SelectableListDialog &dialog, tui::ConsoleRenderer &renderer,
                                                   std::optional<session::SessionId> selection) {
    co_await dialog.wait_until_active();
    if (!selection) co_return dialog.apply(tui::SelectableListAction::CANCEL);

    const auto target = session::to_string(*selection);
    for (usize attempts = 0; attempts < 50 && renderer.selectable_list_selection() != std::optional<std::string_view>{target}; ++attempts) {
        if (auto error = dialog.apply(tui::SelectableListAction::DOWN)) co_return error;
    }
    if (renderer.selectable_list_selection() != std::optional<std::string_view>{target}) {
        co_return lighter::Error::k_invalid_argument;
    }
    co_return dialog.apply(tui::SelectableListAction::CONFIRM);
}

enum struct ResumeExpectation {
    CANCELLED,
    PREPARATION_FAILED,
    SWITCHED,
};

void exercise_resume_command(model::Catalog &models, ToolSet &tools, application::SessionCoordinator &coordinator,
                             session::SessionId target, ResumeExpectation expectation) {
    auto current_model = models.select("test/fallback");
    require(current_model.has_value(), "failed to select current model for resume command fixture");
    session::Session current;
    current.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    current.metadata.working_directory = "workspace";
    const auto current_id = current.id;
    Agent agent(*std::move(current_model), tools, {}, std::move(current));

    lighter::TerminalSession terminal;
    tui::ConsoleRenderer renderer(&terminal);
    renderer.pause_rendering();
    require(!renderer.load_transcript({{.kind = tui::BlockKind::USER, .text = "current transcript"}}),
            "failed to seed rendered current transcript");
    tui::SelectableListDialog dialog;

    lighter::EventLoop loop;
    auto command = tui::resume_session(agent, &coordinator, renderer, dialog);
    auto driver =
        drive_session_picker(dialog, renderer, expectation == ResumeExpectation::CANCELLED ? std::nullopt : std::optional{target});
    loop.schedule(command);
    loop.schedule(driver);
    loop.run();
    require(!driver.result(), "deterministic resume picker driver failed");
    require(!command.result(), "resume command returned a rendering error");

    if (expectation == ResumeExpectation::SWITCHED) {
        require(agent.session.id == target && agent.model.entry.id == "fallback" && !renderer.screen.transcript.blocks.empty() &&
                    renderer.screen.transcript.blocks.front().text.starts_with("projected "),
                "successful /resume did not atomically replace agent identity, model, and rendered transcript");
        return;
    }
    require(agent.session.id == current_id && agent.model.entry.id == "fallback" && renderer.screen.transcript.blocks.size() == 1 &&
                renderer.screen.transcript.blocks.front().text == "current transcript",
            "cancelled or failed /resume changed the live agent or rendered transcript");
}

void test_resume_command_transaction_preserves_or_swaps_complete_state() {
    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open resume command transaction store");
    auto models = test_model_catalog();
    ToolSet tools(database.directory);

    session::Session target;
    target.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    target.metadata.working_directory = "workspace";
    target.start_task("target transcript");
    persist_session(*store, target);

    session::Session corrupt;
    corrupt.metadata.workspace = session::SessionWorkspace{.root = "workspace", .key = "workspace"};
    corrupt.metadata.working_directory = "workspace";
    corrupt.start_task("corrupt transcript");
    persist_session(*store, corrupt);
    sqlite3 *raw = nullptr;
    require(sqlite3_open(database.path.string().c_str(), &raw) == SQLITE_OK, "failed to open resume command corruption fixture");
    sqlite3_stmt *corrupt_payload = nullptr;
    require(sqlite3_prepare_v2(raw, "UPDATE session_entries SET payload_version=99 WHERE session_id=?1", -1, &corrupt_payload, nullptr) ==
                SQLITE_OK,
            "failed to prepare resume command corruption fixture");
    sqlite3_bind_blob(corrupt_payload, 1, corrupt.id.bytes.data(), static_cast<int>(corrupt.id.bytes.size()), SQLITE_TRANSIENT);
    require(sqlite3_step(corrupt_payload) == SQLITE_DONE, "failed to corrupt resume command target");
    sqlite3_finalize(corrupt_payload);
    sqlite3_close(raw);

    application::SessionCoordinator coordinator(*store, preparation_services(models));
    exercise_resume_command(models, tools, coordinator, target.id, ResumeExpectation::CANCELLED);
    {
        auto locked = store->lease(target.id);
        require(locked.has_value(), "failed to hold resume command target lock");
        exercise_resume_command(models, tools, coordinator, target.id, ResumeExpectation::PREPARATION_FAILED);
    }
    exercise_resume_command(models, tools, coordinator, corrupt.id, ResumeExpectation::PREPARATION_FAILED);

    auto projection_services = preparation_services(models);
    projection_services.project = [](const session::Session &) -> Result<std::vector<tui::Block>> {
        return lighter::outcome_error(Error::protocol("injected projection failure"));
    };
    application::SessionCoordinator projection_coordinator(*store, std::move(projection_services));
    exercise_resume_command(models, tools, projection_coordinator, target.id, ResumeExpectation::PREPARATION_FAILED);

    auto model_services = preparation_services(models);
    model_services.resolve_model =
        [](const std::optional<session::SessionModelPreference> &) -> Result<application::SessionModelResolution> {
        return lighter::outcome_error(Error::config("injected model resolution failure"));
    };
    application::SessionCoordinator model_coordinator(*store, std::move(model_services));
    exercise_resume_command(models, tools, model_coordinator, target.id, ResumeExpectation::PREPARATION_FAILED);

    exercise_resume_command(models, tools, coordinator, target.id, ResumeExpectation::SWITCHED);
}

void test_recovery_never_replays_tools() {
    session::Session interrupted;
    const auto task = interrupted.start_task("run tools");
    const auto call = interrupted.next_provider_call();
    interrupted.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "one"}, .call = tool_call("call-one")},
    });
    interrupted.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ToolCallItem{.id = {.value = "two"}, .call = tool_call("call-two")},
    });
    auto recovered = session::recover_interrupted(interrupted);
    require(recovered.unknown_tool_outcomes == 2, "recovery did not identify every unmatched durable tool call");
    require(std::holds_alternative<session::ProviderCallAborted>(interrupted.entries[3].payload) &&
                std::get<session::ProviderCallAborted>(interrupted.entries[3].payload).reason ==
                    session::ProviderCallAbortReason::INTERRUPTED,
            "recovery did not terminate the incomplete provider call");
    const auto &results = std::get<session::ToolResults>(interrupted.entries[4].payload).results;
    require(results.size() == 2 && results[0].call_id == "call-one" && results[1].call_id == "call-two" &&
                results[0].content.contains("will not be retried automatically"),
            "recovery did not synthesize ordered outcome-unknown results");
    require(std::get<session::TaskFinished>(interrupted.entries.back().payload).outcome == session::TaskOutcome::INTERRUPTED,
            "unfinished follow-up state was not marked interrupted");

    session::Session terminal;
    const auto terminal_task = terminal.start_task("answer");
    const auto terminal_call = terminal.next_provider_call();
    terminal.append(session::OutputItemCompleted{
        .task_id = terminal_task,
        .provider_call_id = terminal_call,
        .item = provider::AssistantMessageItem{.id = {.value = "answer"},
                                               .parts = {{.text = "durable reply"}},
                                               .phase = provider::MessagePhase::FINAL},
    });
    terminal.append(session::ProviderCallCompleted{
        .task_id = terminal_task,
        .id = terminal_call,
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    session::recover_interrupted(terminal);
    require(std::get<session::TaskFinished>(terminal.entries.back().payload).outcome == session::TaskOutcome::COMPLETED &&
                terminal.reply_from_latest() == "durable reply",
            "terminal provider call was misclassified or lost its copyable reply");

    const auto recover_abort = [](session::ProviderCallAbortReason reason) {
        session::Session value;
        const auto task = value.start_task("abort");
        const auto call = value.next_provider_call();
        value.append(session::ProviderCallAborted{.task_id = task, .id = call, .reason = reason, .detail = "stopped"});
        session::recover_interrupted(value);
        return std::get<session::TaskFinished>(value.entries.back().payload).outcome;
    };
    require(recover_abort(session::ProviderCallAbortReason::CANCELLED) == session::TaskOutcome::CANCELLED &&
                recover_abort(session::ProviderCallAbortReason::FAILED) == session::TaskOutcome::FAILED,
            "recovery did not preserve durable cancelled and failed outcomes");

    TemporaryDatabase database;
    auto store = session::Store::open(database.path);
    require(store.has_value(), "failed to open recovery restart store");
    session::Session crashed;
    crashed.metadata.working_directory = database.directory.generic_string();
    const auto crashed_task = crashed.start_task("crash after dispatch");
    const auto crashed_call = crashed.next_provider_call();
    crashed.append(session::OutputItemCompleted{
        .task_id = crashed_task,
        .provider_call_id = crashed_call,
        .item = provider::ToolCallItem{.id = {.value = "durable-tool"}, .call = tool_call("durable-call")},
    });
    auto writer = store->lease(crashed.id);
    require(writer && writer->commit(session::make_delta(crashed, crashed.entries)), "failed to persist crash prefix");
    auto restarted = writer->load();
    require(restarted.has_value(), "failed to load crash prefix");
    const auto durable_size = restarted->entries.size();
    auto restart_recovery = session::recover_interrupted(*restarted);
    require(restart_recovery.unknown_tool_outcomes == 1, "restart recovery did not synthesize unknown outcome");
    auto recovery_entries = std::span(restarted->entries).subspan(durable_size);
    require(writer->commit(session::make_delta(*restarted, recovery_entries)).has_value(), "failed to persist recovery suffix");
    auto recovered_restart = writer->load();
    require(recovered_restart && recovered_restart->entries.size() == durable_size + 3 &&
                std::get<session::TaskFinished>(recovered_restart->entries.back().payload).outcome == session::TaskOutcome::INTERRUPTED,
            "recovery suffix did not survive a second restart");
}

void test_session_rejects_mismatched_persistence_queue() {
    session::Session owner;
    session::Session other;
    auto queue = session::PersistenceQueue::create_for_test(owner.id, [](const session::SessionDelta &) -> Result<void> { return {}; });

    require(queue->session_id() == owner.id, "persistence queue did not retain its immutable session identity");
    auto mismatched = other.attach_persistence(queue);
    require(!mismatched && mismatched.error().detail.contains("another session") && other.persistence_queue() == nullptr,
            "session accepted a persistence queue bound to another session");
    require(owner.attach_persistence(queue).has_value() && owner.persistence_queue() == queue.get(),
            "session rejected its matching persistence queue");
    require(!owner.attach_persistence(queue), "session allowed its persistence owner to be replaced");
}

void test_ordered_queue_failure_and_retry() {
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
    require(value.attach_persistence(queue).has_value(), "failed to attach the matching persistence queue");
    const auto task = value.start_task("queue test");
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 1; });
    }
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = {.value = 1},
        .item = provider::AssistantMessageItem{.id = {.value = "message"}, .parts = {{.text = "still live"}}},
    });
    {
        std::scoped_lock lock(gate.mutex);
        gate.release_failure = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 2; });
    }
    const auto degraded = queue->status();
    require(degraded.degraded && degraded.pending_mutations == 2, "failed persistence did not retain and expose the complete unsaved tail");
    {
        std::scoped_lock lock(gate.mutex);
        gate.release_success = true;
    }
    gate.changed.notify_all();
    require(queue->flush().has_value(), "ordered pending prefix did not flush after storage recovered");
    std::scoped_lock lock(gate.mutex);
    require(gate.received.back().entries.size() == 2 && gate.received.back().entries[1].parent_id == gate.received.back().entries[0].id,
            "queue retry did not preserve semantic order and parent links");
}

void test_flush_waits_for_its_complete_pending_prefix() {
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
    require(value.attach_persistence(queue).has_value(), "failed to attach the matching persistence queue");
    const auto task = value.start_task("first mutation");
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.attempts == 1; });
    }
    value.append(session::TaskFinished{.id = task});

    std::jthread flusher([&] {
        const auto result = queue->flush();
        std::scoped_lock lock(gate.mutex);
        gate.flush_returned = true;
        gate.flush_succeeded = result.has_value();
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
        require(!gate.flush_returned, "flush returned after only part of its pending prefix was committed");
        gate.release_second = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.flush_returned; });
        require(gate.flush_succeeded, "flush did not succeed after its complete pending prefix committed");
    }
}

void test_state_path_resolution_failure_retries() {
    TemporaryDatabase database;
    EnvironmentRestore restore("LIMINAL_STATE_DB");
    set_environment("LIMINAL_STATE_DB", database.directory.generic_string());

    session::Session value;
    auto queue = session::PersistenceQueue::create_resolving(value.id, "injected state path failure");
    require(value.attach_persistence(queue).has_value(), "failed to attach the resolving persistence queue");
    value.start_task("retry state path");
    require(queue->status().degraded, "state-path failure did not expose persistent degraded status");
    require(!queue->flush(), "state-path failure unexpectedly persisted a session");

    set_environment("LIMINAL_STATE_DB", database.path.generic_string());
    require(queue->flush().has_value(), "persistence queue did not retry state-path resolution");
    const auto status = queue->status();
    require(!status.degraded && status.pending_mutations == 0, "successful state-path retry did not clear degraded status");
}

void test_transcript_hydration() {
    TemporaryDatabase directory;
    ToolSet tools(directory.directory);
    session::Session value;
    const auto task = value.start_task("hydrate me");
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
        .item = provider::ToolCallItem{.id = {.value = "tool"}, .call = tool_call("hydrate-call")},
    });
    value.append(session::OutputItemCompleted{
        .task_id = task,
        .provider_call_id = call,
        .item = provider::ProviderOpaqueItem{.id = {.value = "opaque"}, .part = {.provider_tag = "openai", .payload = "private"}},
    });
    value.append(session::ToolResults{
        .task_id = task,
        .provider_call_id = call,
        .results = {{.call_id = "hydrate-call", .content = "contents"}},
    });
    value.append(session::ProviderCallAborted{
        .task_id = task,
        .id = call,
        .reason = session::ProviderCallAbortReason::INTERRUPTED,
        .detail = "crashed",
    });
    value.append(session::TaskFinished{.id = task, .outcome = session::TaskOutcome::INTERRUPTED});
    value.append(session::ContextCheckpoint{});

    const auto blocks = tui::project_transcript(value, tools);
    require(blocks.size() == 6 && blocks[0].kind == tui::BlockKind::USER && blocks[1].message_phase == provider::MessagePhase::COMMENTARY &&
                blocks[2].kind == tui::BlockKind::TOOL && blocks[2].state == tui::BlockState::COMPLETED &&
                blocks[3].text.contains("interrupted") && blocks[4].text.contains("interrupted") && blocks[5].text == "History compacted",
            "bulk transcript hydration did not reconstruct stable semantic blocks");
}

i32 run_all() {
    test_uuid_v7();
    test_store_preserves_caller_owned_directory_permissions();
    test_established_database_requires_application_identity();
    test_unidentified_nonempty_database_is_not_adopted();
    test_catalog_index_migrates_from_phase_one_schema();
    test_writer_rejects_updated_at_regression();
    test_windows_workspace_key_uses_unicode_case_mapping();
    test_store_round_trip_and_restart_branching();
    test_unknown_payload_version_isolated_from_catalog();
    test_catalog_keyset_paging();
    test_large_catalog_uses_indexed_keyset_query();
    test_session_catalog_mutations_persist_and_respect_ownership();
    test_durable_acquisition_precedes_live_model_resolution();
    test_prepared_switch_is_atomic_and_explicit_about_unsaved_history();
    test_resume_command_transaction_preserves_or_swaps_complete_state();
    test_recovery_never_replays_tools();
    test_session_rejects_mismatched_persistence_queue();
    test_ordered_queue_failure_and_retry();
    test_flush_waits_for_its_complete_pending_prefix();
    test_state_path_resolution_failure_retries();
    test_transcript_hydration();
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
