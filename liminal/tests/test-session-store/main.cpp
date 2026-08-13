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

#include <liminal/context/context.h>
#include <liminal/session/codec.h>
#include <liminal/session/persistence.h>
#include <liminal/session/paths.h>
#include <liminal/session/recovery.h>
#include <liminal/session/store.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/hydration.h>

namespace {

using namespace lighter::types;
using namespace liminal;

static_assert(!std::default_initializable<session::Store>);
static_assert(!std::default_initializable<session::SessionWriter>);

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
    const auto wal = std::filesystem::path(database.path.string() + "-wal");
    require(std::filesystem::exists(wal) &&
                (std::filesystem::status(wal).permissions() & public_permissions) == std::filesystem::perms::none,
            "SQLite WAL sidecar was created with public permissions");
#endif
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
    require(latest && latest->id == value.id, "unknown entry payload version made the catalog unlistable");
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

void test_ordered_queue_failure_and_retry() {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        usize attempts = 0;
        bool release_failure = false;
        bool release_success = false;
        std::vector<session::SessionDelta> received;
    } gate;

    auto queue = session::PersistenceQueue::create_for_test([&gate](const session::SessionDelta &delta) -> Result<void> {
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
    session::Session value;
    value.attach_persistence(queue);
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

    auto queue = session::PersistenceQueue::create_for_test([&gate](const session::SessionDelta &) -> Result<void> {
        std::unique_lock lock(gate.mutex);
        ++gate.attempts;
        const auto attempt = gate.attempts;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate, attempt] { return attempt == 1 ? gate.release_first : gate.release_second; });
        return {};
    });
    session::Session value;
    value.attach_persistence(queue);
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
    value.attach_persistence(queue);
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
    test_windows_workspace_key_uses_unicode_case_mapping();
    test_store_round_trip_and_restart_branching();
    test_unknown_payload_version_isolated_from_catalog();
    test_recovery_never_replays_tools();
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
