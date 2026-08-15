#include "session_coordinator.h"

#include <algorithm>
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

} // namespace

SessionPreparationServices::SessionPreparationServices(TranscriptProjector project, ModelResolver resolve_model,
                                                       PersistenceFactory persistence, PersistenceFactory unpublished_persistence)
    : persistence(std::move(persistence)), unpublished_persistence(std::move(unpublished_persistence)), project(std::move(project)),
      resolve_model(std::move(resolve_model)) {}

Result<SessionModelResolution> resolve_session_model(model::Catalog &models, const std::optional<std::string> &configured_model,
                                                     const std::optional<session::SessionModelPreference> &stored_model) {
    if (configured_model) {
        auto selected = models.select(*configured_model);
        if (!selected) return lighter::outcome_error(std::move(selected).error());
        return SessionModelResolution{.model = *std::move(selected)};
    }
    if (models.entries().empty()) return lighter::outcome_error(Error::config("no configured models"));

    const auto &fallback = models.entries().front();
    const auto fallback_selector = fallback.provider + "/" + fallback.id;
    if (!stored_model) {
        auto selected = models.select(fallback_selector);
        if (!selected) return lighter::outcome_error(std::move(selected).error());
        return SessionModelResolution{.model = *std::move(selected)};
    }

    const auto stored_selector = stored_model_selector(*stored_model);
    auto selected = models.select(stored_selector);
    if (selected) return SessionModelResolution{.model = *std::move(selected)};

    auto fallback_choice = models.select(fallback_selector);
    if (!fallback_choice) return lighter::outcome_error(std::move(fallback_choice).error());
    return SessionModelResolution{
        .model = *std::move(fallback_choice),
        .notice = "[stored model " + stored_selector + " is unavailable; using " + fallback_selector + "]\n",
    };
}

SessionCoordinator::SessionCoordinator(session::SessionRepository repository, session::SessionCatalog catalog,
                                       SessionPreparationServices services)
    : repository(std::move(repository)), catalog(std::move(catalog)), services(std::move(services)) {
    lighter::check(static_cast<bool>(this->services.project), "session preparation requires a transcript projector");
    lighter::check(static_cast<bool>(this->services.resolve_model), "session preparation requires a model resolver");
    if (!this->services.persistence) {
        this->services.persistence = [](session::SessionWriter writer) { return session::PersistenceQueue::create(std::move(writer)); };
    }
    if (!this->services.unpublished_persistence) {
        this->services.unpublished_persistence = [](session::SessionWriter writer) {
            return session::PersistenceQueue::create_unpublished(std::move(writer));
        };
    }
}

Result<session::SessionPage> SessionCoordinator::page(const session::SessionPageQuery &query) const { return catalog.page(query); }

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

PreparedSession SessionSwitch::take_target() && {
    lighter::check(target.has_value(), "ready session switch has no prepared target");
    auto prepared = std::move(*target);
    target.reset();
    current_state = SessionSwitchState::CONSUMED;
    return prepared;
}

Result<AcquiredSession> SessionCoordinator::acquire(session::SessionId id) const { return acquire_with_workspace(id, std::nullopt); }

Result<AcquiredSession> SessionCoordinator::acquire_in_workspace(session::SessionId id, std::string_view workspace_key) const {
    return acquire_with_workspace(id, std::string(workspace_key));
}

Result<AcquiredSession> SessionCoordinator::acquire_with_workspace(session::SessionId id,
                                                                   const std::optional<std::string> &workspace_key) const {
    auto writer = repository.acquire(id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto loaded = writer->load();
    if (!loaded) return lighter::outcome_error(std::move(loaded).error());
    if (workspace_key && (!loaded->metadata.workspace || loaded->metadata.workspace->key != *workspace_key)) {
        static_cast<void>(writer->refresh_catalog());
        return lighter::outcome_error(Error::storage("selected catalog row does not match the session's immutable workspace"));
    }

    AcquiredSession acquired{.session = *std::move(loaded)};
    auto queue = services.persistence(*std::move(writer));
    auto attached = acquired.session.attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());

    const auto recovered = session::recover_interrupted(acquired.session);
    if (recovered.recovered_tasks != 0) {
        acquired.notices.push_back("[recovered an interrupted task; tools were not re-executed]\n");
        auto flushed = queue->flush();
        if (!flushed) acquired.notices.push_back("[session not saving: " + flushed.error().message() + "]\n");
    }

    auto transcript = services.project(acquired.session);
    if (!transcript) return lighter::outcome_error(std::move(transcript).error());
    acquired.transcript = *std::move(transcript);
    return acquired;
}

Result<PreparedSession> SessionCoordinator::resolve_model(AcquiredSession acquired) const {
    auto resolved = services.resolve_model(acquired.session.metadata.model_preference);
    if (!resolved) return lighter::outcome_error(std::move(resolved).error());
    if (resolved->notice) acquired.notices.push_back(std::move(*resolved->notice));
    return PreparedSession{
        .session = std::move(acquired.session),
        .model = std::move(resolved->model),
        .transcript = std::move(acquired.transcript),
        .notices = std::move(acquired.notices),
    };
}

Result<PreparedSession> SessionCoordinator::prepare(session::SessionId id) const {
    auto acquired = acquire(id);
    if (!acquired) return lighter::outcome_error(std::move(acquired).error());
    return resolve_model(*std::move(acquired));
}

Result<PreparedSession> SessionCoordinator::prepare_catalog_hint(session::SessionId id) const {
    auto hint = catalog.find(id);
    if (!hint) return lighter::outcome_error(std::move(hint).error());
    if (!*hint) return lighter::outcome_error(Error::storage("selected session is no longer present in the catalog"));
    auto acquired = acquire_with_workspace(id, (*hint)->workspace_key);
    if (!acquired) {
        if (acquired.error().detail == "session was not found" || acquired.error().detail == "published session database is absent") {
            static_cast<void>(catalog.remove(id));
        }
        return lighter::outcome_error(std::move(acquired).error());
    }
    return resolve_model(*std::move(acquired));
}

Result<ForkPlan> SessionCoordinator::prepare_fork(const session::Session &source, session::ConversationCheckpointId checkpoint) const {
    auto fork = source.fork_at(checkpoint);
    if (!fork) return lighter::outcome_error(std::move(fork).error());
    auto writer = repository.stage(fork->id, session::make_delta(*fork, fork->entries));
    if (!writer) return lighter::outcome_error(std::move(writer).error());

    AcquiredSession acquired{.session = *std::move(fork)};
    auto queue = services.unpublished_persistence(*std::move(writer));
    auto attached = acquired.session.attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());
    auto transcript = services.project(acquired.session);
    if (!transcript) return lighter::outcome_error(std::move(transcript).error());
    acquired.transcript = *std::move(transcript);
    auto prepared = resolve_model(std::move(acquired));
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    return ForkPlan(*std::move(prepared));
}

Result<PreparedSession> SessionCoordinator::publish_fork(ForkPlan plan) const {
    auto *queue = plan.target.session.persistence_queue();
    if (!queue) return lighter::outcome_error(Error::protocol("prepared fork has no persistence queue"));
    plan.target.session.metadata.updated_at_ms = std::max(plan.target.session.metadata.updated_at_ms, session::unix_milliseconds_now());
    auto published = queue->publish_initial(session::make_delta(plan.target.session, plan.target.session.entries));
    if (!published) return lighter::outcome_error(std::move(published).error());
    return std::move(plan.target);
}

Result<SessionSwitch> SessionCoordinator::begin_switch(session::SessionId current, session::SessionId target) const {
    if (current == target) return SessionSwitch(SessionSwitchState::CURRENT_SELECTED);
    auto prepared = prepare_catalog_hint(target);
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    return SessionSwitch(SessionSwitchState::PREPARED, *std::move(prepared));
}

Result<session::PersistenceStatus> SessionCoordinator::rename_inactive(session::SessionId id, RenameSession mutation) const {
    auto writer = repository.acquire(id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto loaded = writer->load();
    if (!loaded) return lighter::outcome_error(std::move(loaded).error());
    auto queue = services.persistence(*std::move(writer));
    auto attached = loaded->attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());
    loaded->set_title(std::move(mutation.title));
    auto flushed = queue->flush();
    if (!flushed) return lighter::outcome_error(std::move(flushed).error());
    return queue->status();
}

} // namespace liminal::application
