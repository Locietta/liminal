#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/model/catalog.h>
#include <liminal/session/persistence.h>
#include <liminal/session/catalog.h>
#include <liminal/session/repository.h>
#include <liminal/tui/transcript.h>

namespace liminal::application {

struct RenameSession {
    std::optional<std::string> title;
};

struct PreparedSession {
    session::Session session;
    model::Choice model;
    std::vector<tui::Block> transcript;
    std::vector<std::string> notices;
};

/// An unpublished, fully prepared fork. Destroying the plan releases its
/// target lease without adding a session to the durable catalog.
struct ForkPlan {
    ForkPlan(ForkPlan &&) noexcept = default;
    ForkPlan &operator=(ForkPlan &&) noexcept = default;
    ForkPlan(const ForkPlan &) = delete;
    ForkPlan &operator=(const ForkPlan &) = delete;

    session::SessionId target_id() const noexcept { return target.session.id; }

private:
    friend struct SessionCoordinator;
    explicit ForkPlan(PreparedSession target) : target(std::move(target)) {}

    PreparedSession target;
};

struct AcquiredSession {
    session::Session session;
    std::vector<tui::Block> transcript;
    std::vector<std::string> notices;
};

struct SessionModelResolution {
    model::Choice model;
    std::optional<std::string> notice;
};

struct SessionPreparationServices {
    using PersistenceFactory = std::copyable_function<std::shared_ptr<session::PersistenceQueue>(session::SessionWriter) const>;
    using TranscriptProjector = std::copyable_function<Result<std::vector<tui::Block>>(const session::Session &) const>;
    using ModelResolver =
        std::copyable_function<Result<SessionModelResolution>(const std::optional<session::SessionModelPreference> &) const>;

    SessionPreparationServices(TranscriptProjector project, ModelResolver resolve_model, PersistenceFactory persistence = {},
                               PersistenceFactory unpublished_persistence = {});

    PersistenceFactory persistence;
    PersistenceFactory unpublished_persistence;
    TranscriptProjector project;
    ModelResolver resolve_model;
};

Result<SessionModelResolution> resolve_session_model(model::Catalog &models, const std::optional<std::string> &configured_model,
                                                     const std::optional<session::SessionModelPreference> &stored_model);

enum struct SessionSwitchState {
    CURRENT_SELECTED,
    PREPARED,
    AWAITING_UNSAVED_CONFIRMATION,
    READY,
    CANCELLED,
    CONSUMED,
};

enum struct UnsavedSwitchDecision {
    STAY,
    ABANDON_UNSAVED_HISTORY,
};

struct SessionSwitch {
    SessionSwitchState state() const noexcept { return current_state; }
    bool abandoned_unsaved_history() const noexcept { return abandoned_history; }

    void flush_current(session::PersistenceQueue *queue) pre(current_state == SessionSwitchState::PREPARED);
    void resolve_unsaved(UnsavedSwitchDecision decision, const session::PersistenceStatus &current_status)
        pre(current_state == SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION);
    PreparedSession take_target() && pre(current_state == SessionSwitchState::READY);

private:
    friend struct SessionCoordinator;
    SessionSwitch(SessionSwitchState state, std::optional<PreparedSession> target = {}) : current_state(state), target(std::move(target)) {}

    SessionSwitchState current_state;
    std::optional<PreparedSession> target;
    bool abandoned_history = false;
};

/// Coordinates ownership, semantic recovery, transcript projection, and model
/// resolution before a saved session can replace a live application session.
struct SessionCoordinator {
    SessionCoordinator(session::SessionRepository repository, SessionPreparationServices services);

    Result<session::SessionPage> page(const session::SessionPageQuery &query) const;
    Result<AcquiredSession> acquire(session::SessionId id) const;
    Result<AcquiredSession> acquire_in_workspace(session::SessionId id, std::string_view workspace_key) const;
    Result<AcquiredSession> acquire_catalog_hint(session::SessionId id) const;
    Result<AcquiredSession> acquire_latest(std::string_view workspace_key) const;
    Result<PreparedSession> resolve_model(AcquiredSession acquired) const;
    Result<PreparedSession> prepare(session::SessionId id) const;
    Result<ForkPlan> prepare_fork(const session::Session &source, session::ConversationCheckpointId checkpoint) const;
    Result<PreparedSession> publish_fork(ForkPlan plan) const;
    Result<SessionSwitch> begin_switch(session::SessionId current, session::SessionId target) const;
    Result<session::PersistenceStatus> rename_inactive(session::SessionId id, RenameSession mutation) const;

private:
    Result<AcquiredSession> acquire_with_workspace(session::SessionId id, const std::optional<std::string> &workspace_key) const;
    Result<PreparedSession> prepare_catalog_hint(session::SessionId id) const;
    session::SessionRepository repository;
    SessionPreparationServices services;
};

} // namespace liminal::application
