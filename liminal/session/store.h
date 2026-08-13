#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

struct SessionDelta {
    std::vector<SessionEntry> entries;
    std::optional<EntryId> active_leaf;
    u64 next_entry_id = 1;
    u64 next_task_id = 1;
    u64 next_provider_call_id = 1;
    u64 entry_count = 0;
    u64 tokens_used = 0;
    SessionMetadata metadata;
};

struct SessionSummary {
    SessionId id;
    i64 created_at_ms = 0;
    i64 updated_at_ms = 0;
    std::optional<std::string> workspace_root;
    std::optional<std::string> title;
    std::string preview;
    std::optional<SessionModelPreference> model_preference;
    u64 entry_count = 0;
    u64 tokens_used = 0;
};

struct SessionWriter;

struct Store {
    struct State;

    static Result<Store> open(std::filesystem::path database_path);

    const std::filesystem::path &path() const noexcept;
    Result<SessionWriter> lease(SessionId id) const;
    Result<SessionId> resolve_id(std::string_view text) const;
    Result<SessionSummary> latest(std::string_view workspace_key) const;

private:
    explicit Store(std::shared_ptr<State> state) : state(std::move(state)) {}
    std::shared_ptr<State> state;
};

struct SessionWriter {
    struct State;

    ~SessionWriter();
    SessionWriter(SessionWriter &&) noexcept;
    SessionWriter &operator=(SessionWriter &&) noexcept;
    SessionWriter(const SessionWriter &) = delete;
    SessionWriter &operator=(const SessionWriter &) = delete;

    SessionId session_id() const noexcept;
    Result<Session> load();
    Result<void> commit(const SessionDelta &delta);

private:
    friend Store;
    explicit SessionWriter(std::shared_ptr<State> state) : state(std::move(state)) {}
    std::shared_ptr<State> state;
};

SessionDelta make_delta(const Session &session, std::span<const SessionEntry> entries);

} // namespace liminal::session
