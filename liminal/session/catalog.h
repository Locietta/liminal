#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

struct SessionRepository;

struct SessionSummary {
    SessionId id;
    i64 updated_at_ms = 0;
    std::optional<std::string> title;
    std::string preview;
};

struct CatalogProjection {
    SessionSummary summary;
    u64 observed_revision = 0;
    std::string workspace_key;
};

struct SessionPageCursor {
    i64 updated_at_ms = 0;
    SessionId id;
    auto operator<=>(const SessionPageCursor &) const = default;
};

struct SessionPageQuery {
    std::string workspace_key;
    std::optional<SessionPageCursor> after;
    usize limit = 50;
};

struct SessionPage {
    std::vector<SessionSummary> sessions;
    std::optional<SessionPageCursor> continuation;
};

struct SessionCatalog {
    struct State;

    const std::filesystem::path &path() const noexcept;
    Result<SessionId> resolve_prefix(std::string_view text) const;
    Result<SessionSummary> latest(std::string_view workspace_key) const;
    Result<SessionPage> page(const SessionPageQuery &query) const pre(query.limit > 0);
    Result<std::optional<CatalogProjection>> find(SessionId id) const;
    Result<std::vector<SessionId>> ids() const;
    Result<void> upsert(const CatalogProjection &projection) const;
    Result<void> remove(SessionId id) const;

private:
    friend struct SessionRepository;
    static Result<SessionCatalog> open(const std::filesystem::path &state_root);
    static Result<SessionCatalog> repair_corrupt(const std::filesystem::path &state_root);
    explicit SessionCatalog(std::shared_ptr<State> state) : state(std::move(state)) {}
    bool owns_rebuild_exclusivity() const noexcept;
    Result<void> complete_rebuild() const;
    std::shared_ptr<State> state;
};

} // namespace liminal::session
