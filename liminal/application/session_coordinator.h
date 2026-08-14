#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <liminal/model/catalog.h>
#include <liminal/session/persistence.h>
#include <liminal/session/store.h>
#include <liminal/tui/transcript.h>

namespace liminal::application {

struct RenameSession {
    std::optional<std::string> title;
};

struct ArchiveSession {};
struct UnarchiveSession {};
using SessionCatalogMutation = std::variant<RenameSession, ArchiveSession, UnarchiveSession>;

struct PreparedSession {
    session::Session session;
    model::Choice model;
    std::vector<tui::Block> transcript;
    std::vector<std::string> notices;
};

struct SessionPreparationServices {
    model::Catalog *models = nullptr;
    std::optional<std::string> configured_model;
    std::string fallback_model;
    std::copyable_function<std::shared_ptr<session::PersistenceQueue>(session::SessionWriter) const> persistence;
    std::copyable_function<Result<std::vector<tui::Block>>(const session::Session &) const> project;
};

enum struct SessionSwitchState {
    CURRENT_SELECTED,
    PREPARED,
    AWAITING_UNSAVED_CONFIRMATION,
    READY,
    CANCELLED,
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
    PreparedSession take_target() pre(current_state == SessionSwitchState::READY);

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
    SessionCoordinator(session::Store store, SessionPreparationServices services);

    Result<session::SessionPage> page(const session::SessionPageQuery &query) const;
    Result<PreparedSession> prepare(session::SessionId id) const;
    Result<SessionSwitch> begin_switch(session::SessionId current, session::SessionId target) const;
    Result<void> mutate_inactive(session::SessionId id, const SessionCatalogMutation &mutation) const;

private:
    session::Store store;
    SessionPreparationServices services;
};

} // namespace liminal::application
