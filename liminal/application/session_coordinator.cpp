#include "session_coordinator.h"

#include <type_traits>
#include <utility>

#include <lighter/async/vocab/outcome.h>
#include <lighter/utils/panic.h>

#include <liminal/session/persistence.h>
#include <liminal/session/recovery.h>

namespace liminal::application {

namespace {

std::string stored_model_selector(const session::SessionModelPreference &preference) {
    auto selector = preference.provider + "/" + preference.model;
    if (preference.reasoning_effort) selector += "@" + *preference.reasoning_effort;
    return selector;
}

void apply_mutation(session::Session &value, const SessionCatalogMutation &mutation) {
    std::visit(
        [&value](const auto &change) {
            using T = std::remove_cvref_t<decltype(change)>;
            if constexpr (std::same_as<T, RenameSession>) {
                value.set_title(change.title);
            } else if constexpr (std::same_as<T, ArchiveSession>) {
                value.archive();
            } else if constexpr (std::same_as<T, UnarchiveSession>) {
                value.unarchive();
            }
        },
        mutation);
}

} // namespace

SessionCoordinator::SessionCoordinator(session::Store store, SessionPreparationServices services)
    : store(std::move(store)), services(std::move(services)) {
    if (!this->services.persistence) {
        this->services.persistence = [](session::SessionWriter writer) { return session::PersistenceQueue::create(std::move(writer)); };
    }
}

Result<session::SessionPage> SessionCoordinator::page(const session::SessionPageQuery &query) const { return store.page(query); }

void SessionSwitch::flush_current(session::PersistenceQueue *queue) {
    if (!queue) {
        current_state = SessionSwitchState::READY;
        return;
    }
    auto flushed = queue->flush();
    current_state = flushed ? SessionSwitchState::READY : SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION;
}

void SessionSwitch::resolve_unsaved(UnsavedSwitchDecision decision, const session::PersistenceStatus &current_status) {
    if (decision == UnsavedSwitchDecision::STAY) {
        current_state = SessionSwitchState::CANCELLED;
        return;
    }
    abandoned_history = current_status.pending_mutations != 0;
    current_state = SessionSwitchState::READY;
}

PreparedSession SessionSwitch::take_target() {
    lighter::check(target.has_value(), "ready session switch has no prepared target");
    return std::move(*target);
}

Result<PreparedSession> SessionCoordinator::prepare(session::SessionId id) const {
    auto writer = store.lease(id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto loaded = writer->load();
    if (!loaded) return lighter::outcome_error(std::move(loaded).error());

    PreparedSession prepared{.session = *std::move(loaded)};
    auto queue = services.persistence(*std::move(writer));
    auto attached = prepared.session.attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());

    const auto recovered = session::recover_interrupted(prepared.session);
    if (recovered.recovered_tasks != 0) {
        prepared.notices.push_back("[recovered an interrupted task; tools were not re-executed]\n");
        auto flushed = queue->flush();
        if (!flushed) prepared.notices.push_back("[session not saving: " + flushed.error().message() + "]\n");
    }

    auto transcript = services.project(prepared.session);
    if (!transcript) return lighter::outcome_error(std::move(transcript).error());
    prepared.transcript = *std::move(transcript);

    const bool use_stored_model = !services.configured_model && prepared.session.metadata.model_preference.has_value();
    auto selector = services.configured_model.value_or(
        use_stored_model ? stored_model_selector(*prepared.session.metadata.model_preference) : services.fallback_model);
    auto selected = services.models->select(selector);
    if (!selected && use_stored_model) {
        auto fallback = services.models->select(services.fallback_model);
        if (!fallback) return lighter::outcome_error(std::move(fallback).error());
        prepared.notices.push_back("[stored model " + selector + " is unavailable; using " + services.fallback_model + "]\n");
        selected = *std::move(fallback);
    }
    if (!selected) return lighter::outcome_error(std::move(selected).error());
    prepared.model = *std::move(selected);
    return prepared;
}

Result<SessionSwitch> SessionCoordinator::begin_switch(session::SessionId current, session::SessionId target) const {
    if (current == target) return SessionSwitch(SessionSwitchState::CURRENT_SELECTED);
    auto prepared = prepare(target);
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    return SessionSwitch(SessionSwitchState::PREPARED, *std::move(prepared));
}

Result<void> SessionCoordinator::mutate_inactive(session::SessionId id, const SessionCatalogMutation &mutation) const {
    auto writer = store.lease(id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto loaded = writer->load();
    if (!loaded) return lighter::outcome_error(std::move(loaded).error());
    auto queue = services.persistence(*std::move(writer));
    auto attached = loaded->attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());
    apply_mutation(*loaded, mutation);
    return queue->flush();
}

} // namespace liminal::application
