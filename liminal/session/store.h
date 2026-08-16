#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

struct CatalogProjection;

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

struct SessionCommitResult {
    std::optional<std::string> catalog_degradation;
};

struct CatalogRefreshStatus {
    bool degraded = false;
    std::string detail;
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
    Result<SessionCommitResult> commit(const SessionDelta &delta);
    Result<void> refresh_catalog();
    CatalogRefreshStatus catalog_status() const;
    bool publication_attachment_pending() const;

private:
    friend struct SessionRepository;
    Result<void> stage_initial(const SessionDelta &delta);
    Result<CatalogProjection> projection() const;
    explicit SessionWriter(std::shared_ptr<State> state) : state(std::move(state)) {}
    std::shared_ptr<State> state;
};

SessionDelta make_delta(const Session &session, std::span<const SessionEntry> entries);

} // namespace liminal::session
