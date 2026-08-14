#include "session_commands.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::tui {

namespace {

std::string relative_update_time(i64 updated_at_ms, i64 now_ms) {
    const auto elapsed = std::max(now_ms - updated_at_ms, i64{0});
    if (elapsed < 60'000) return "just now";
    if (elapsed < 3'600'000) return std::to_string(elapsed / 60'000) + "m ago";
    if (elapsed < 86'400'000) return std::to_string(elapsed / 3'600'000) + "h ago";
    return std::to_string(elapsed / 86'400'000) + "d ago";
}

SelectableListPage picker_page(const session::SessionPage &page, i64 now_ms) {
    SelectableListPage result{.has_more = page.continuation.has_value()};
    result.items.reserve(page.sessions.size());
    for (const auto &summary : page.sessions) {
        auto primary = summary.title.value_or(summary.preview.empty() ? "Untitled session" : summary.preview);
        auto secondary = relative_update_time(summary.updated_at_ms, now_ms);
        if (summary.model_preference) secondary += " · " + summary.model_preference->provider + "/" + summary.model_preference->model;
        secondary += " · " + session::to_string(summary.id).substr(0, 8);
        result.items.push_back({.id = session::to_string(summary.id), .primary = std::move(primary), .secondary = std::move(secondary)});
    }
    return result;
}

Result<void> open_session_picker(SelectableListDialog &dialog, ConsoleRenderer &renderer, application::SessionCoordinator &sessions,
                                 std::string workspace_key, session::SessionCatalogState state, std::string title,
                                 std::string empty_message) {
    constexpr usize k_picker_page_size = 10;
    auto first = sessions.page({.workspace_key = workspace_key, .state = state, .limit = k_picker_page_size});
    if (!first) return lighter::outcome_error(std::move(first).error());
    const auto now_ms = session::unix_milliseconds_now();
    auto cursor = first->continuation;
    SelectableList list(std::move(title), std::move(empty_message), picker_page(*first, now_ms));
    SelectableListDialog::LoadPage load = [&sessions, workspace_key = std::move(workspace_key), state, cursor,
                                           now_ms]() mutable -> Result<SelectableListPage> {
        auto next = sessions.page({.workspace_key = workspace_key, .state = state, .after = cursor, .limit = k_picker_page_size});
        if (!next) return lighter::outcome_error(std::move(next).error());
        cursor = next->continuation;
        return picker_page(*next, now_ms);
    };
    if (auto error = dialog.begin(renderer, std::move(list), std::move(load))) {
        return lighter::outcome_error(Error::protocol("cannot render session picker: " + std::string(error.message())));
    }
    return {};
}

Result<void> open_unsaved_confirmation(SelectableListDialog &dialog, ConsoleRenderer &renderer) {
    SelectableList list("Switch away from unsaved history?", "",
                        SelectableListPage{.items = {
                                               {.id = "stay", .primary = "Stay in current session"},
                                               {.id = "switch", .primary = "Switch and abandon the unsaved history"},
                                           }});
    list.description = "The unsaved tail will not be resumable; external tool effects are not undone.";
    if (auto error = dialog.begin(renderer, std::move(list))) {
        return lighter::outcome_error(Error::protocol("cannot render switch confirmation: " + std::string(error.message())));
    }
    return {};
}

Result<void> mutate_selected_session(Agent &agent, application::SessionCoordinator &sessions, session::SessionId id,
                                     const application::SessionCatalogMutation &mutation) {
    if (id != agent.session.id) return sessions.mutate_inactive(id, mutation);
    std::visit(
        [&agent](const auto &change) {
            using T = std::remove_cvref_t<decltype(change)>;
            if constexpr (std::same_as<T, application::RenameSession>) {
                agent.session.set_title(change.title);
            } else if constexpr (std::same_as<T, application::ArchiveSession>) {
                agent.session.archive();
            } else if constexpr (std::same_as<T, application::UnarchiveSession>) {
                agent.session.unarchive();
            }
        },
        mutation);
    auto *queue = agent.session.persistence_queue();
    return queue ? queue->flush() : Result<void>{};
}

} // namespace

lighter::Task<lighter::Error> resume_session(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                             SelectableListDialog &dialog) {
    if (!sessions || !renderer.terminal || !agent.session.metadata.workspace) {
        co_return renderer.status("Resume picker is unavailable in this session");
    }
    auto opened = open_session_picker(dialog, renderer, *sessions, agent.session.metadata.workspace->key,
                                      session::SessionCatalogState::ACTIVE, "Resume session", "No resumable sessions in this workspace");
    if (!opened) co_return renderer.status("Resume error: " + opened.error().message());
    auto choice = co_await dialog.next();
    if (!choice) co_return lighter::Error{};
    auto id = session::parse_session_id(*choice);
    if (!id) co_return renderer.status("Resume error: " + id.error().message());

    auto switching = sessions->begin_switch(agent.session.id, *id);
    if (!switching) co_return renderer.status("Resume error: " + switching.error().message());
    if (switching->state() == application::SessionSwitchState::CURRENT_SELECTED) {
        co_return renderer.status("Already in the selected session");
    }

    auto *current_queue = agent.session.persistence_queue();
    switching->flush_current(current_queue);
    if (switching->state() == application::SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION) {
        auto confirmation = open_unsaved_confirmation(dialog, renderer);
        if (!confirmation) co_return renderer.status("Resume error: " + confirmation.error().message());
        auto decision = co_await dialog.next();
        switching->resolve_unsaved(decision && *decision == "switch" ? application::UnsavedSwitchDecision::ABANDON_UNSAVED_HISTORY :
                                                                       application::UnsavedSwitchDecision::STAY,
                                   current_queue->status());
    }
    if (switching->state() == application::SessionSwitchState::CANCELLED) co_return lighter::Error{};
    const auto abandoned_unsaved_history = switching->abandoned_unsaved_history();
    auto prepared = std::move(*switching).take_target();
    if (auto error = renderer.load_transcript(std::move(prepared.transcript))) co_return error;
    const auto resumed_id = session::to_string(prepared.session.id).substr(0, 8);
    agent.replace_session(std::move(prepared.model), std::move(prepared.session));
    if (auto error = renderer.render(ModelSelected{.name = agent.model.entry.id, .effort = agent.model.reasoning_effort})) co_return error;
    for (const auto &notice : prepared.notices) {
        if (auto error = renderer.notice(notice)) co_return error;
    }
    if (abandoned_unsaved_history) {
        if (auto error = renderer.notice("[switched sessions; the old unsaved history was abandoned and external tool effects were not "
                                         "undone]\n"))
            co_return error;
    }
    co_return renderer.status("Resumed session " + resumed_id);
}

lighter::Task<lighter::Error> change_archive_state(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                                   SelectableListDialog &dialog, ArchiveCommand command) {
    if (!sessions || !renderer.terminal || !agent.session.metadata.workspace) {
        co_return renderer.notice("[session catalog is unavailable in this session]\n");
    }
    const bool archiving = command == ArchiveCommand::ARCHIVE;
    auto opened = open_session_picker(dialog, renderer, *sessions, agent.session.metadata.workspace->key,
                                      archiving ? session::SessionCatalogState::ACTIVE : session::SessionCatalogState::ARCHIVED,
                                      archiving ? "Archive session" : "Unarchive session",
                                      archiving ? "No active sessions in this workspace" : "No archived sessions in this workspace");
    if (!opened) co_return renderer.notice("[archive error: " + opened.error().message() + "]\n");
    auto choice = co_await dialog.next();
    if (!choice) co_return lighter::Error{};
    auto id = session::parse_session_id(*choice);
    if (!id) co_return renderer.notice("[archive error: " + id.error().message() + "]\n");
    application::SessionCatalogMutation mutation = archiving ? application::SessionCatalogMutation{application::ArchiveSession{}} :
                                                               application::SessionCatalogMutation{application::UnarchiveSession{}};
    auto changed = mutate_selected_session(agent, *sessions, *id, mutation);
    if (!changed) co_return renderer.notice("[archive error: " + changed.error().message() + "]\n");
    co_return renderer.status(archiving ? "Session archived" : "Session unarchived");
}

lighter::Error name_current_session(Agent &agent, ConsoleRenderer &renderer, std::optional<std::string> title) {
    agent.session.set_title(std::move(title));
    auto *queue = agent.session.persistence_queue();
    auto saved = queue ? queue->flush() : Result<void>{};
    if (saved) return renderer.status("Session name updated");
    return renderer.notice("[Session name changed in memory; saving failed]\n");
}

} // namespace liminal::tui
