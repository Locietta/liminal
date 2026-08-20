#include "session_commands.h"

#include <algorithm>
#include <charconv>
#include <utility>

#include <lighter/async/vocab/outcome.h>

#include <liminal/text.h>
#include <liminal/tui/hydration.h>
#include <liminal/tui/picker_query.h>

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
    SelectableListPage result{.has_previous = page.preceding.has_value(), .has_more = page.continuation.has_value()};
    result.items.reserve(page.sessions.size());
    for (const auto &summary : page.sessions) {
        auto primary = summary.title.value_or(summary.preview.empty() ? "Untitled session" : summary.preview);
        auto secondary = relative_update_time(summary.updated_at_ms, now_ms);
        secondary += " · " + session::to_string(summary.id).substr(0, 8);
        result.items.push_back({.id = session::to_string(summary.id), .primary = std::move(primary), .secondary = std::move(secondary)});
    }
    return result;
}

Result<void> open_session_picker(SelectableListDialog &dialog, ConsoleRenderer &renderer, application::SessionCoordinator &sessions,
                                 std::string workspace_key, std::string title, std::string empty_message) {
    constexpr usize k_picker_page_size = 10;
    auto first = sessions.page({.workspace_key = workspace_key, .limit = k_picker_page_size});
    if (!first) return lighter::outcome_error(std::move(first).error());
    const auto now_ms = session::unix_milliseconds_now();
    auto preceding = first->preceding;
    auto continuation = first->continuation;
    SelectableList list(std::move(title), std::move(empty_message), picker_page(*first, now_ms));
    list.set_contextual_header({.identity = "Resume Session", .session = renderer.screen.header});
    list.enable_query("No matching sessions in this workspace");
    SelectableListDialog::LoadPage load = [&sessions, workspace_key = std::move(workspace_key), preceding, continuation,
                                           loaded_query = std::string{},
                                           now_ms](std::string_view query, SelectableListPageLoad load,
                                                   std::optional<std::string_view> preferred_id) mutable -> Result<SelectableListPage> {
        if (load != SelectableListPageLoad::REPLACE && query != loaded_query) {
            return lighter::outcome_error(Error::protocol("session picker paging query changed unexpectedly"));
        }
        std::optional<session::SessionId> preferred;
        if (load == SelectableListPageLoad::REPLACE && preferred_id) {
            auto parsed = session::parse_session_id(*preferred_id);
            if (!parsed) return lighter::outcome_error(Error::protocol("session picker selected an invalid session identity"));
            preferred = *parsed;
        }
        session::SessionPageQuery request{.workspace_key = workspace_key,
                                          .query = std::string(query),
                                          .preferred_id = preferred,
                                          .before = load == SelectableListPageLoad::PREVIOUS ? preceding : std::nullopt,
                                          .after = load == SelectableListPageLoad::NEXT ? continuation : std::nullopt,
                                          .limit = k_picker_page_size};
        auto next = sessions.page(request);
        if (!next) return lighter::outcome_error(std::move(next).error());
        loaded_query = query;
        if (load == SelectableListPageLoad::REPLACE) {
            preceding = next->preceding;
            continuation = next->continuation;
        } else if (load == SelectableListPageLoad::PREVIOUS) {
            preceding = next->preceding;
        } else {
            continuation = next->continuation;
        }
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

Result<void> open_unsaved_fork_confirmation(SelectableListDialog &dialog, ConsoleRenderer &renderer) {
    SelectableList list("Fork while the source is not saved?", "",
                        SelectableListPage{.items = {
                                               {.id = "stay", .primary = "Stay and cancel the fork"},
                                               {.id = "fork", .primary = "Create the fork and switch"},
                                           }});
    list.description = "The selected prefix will be saved in the fork. Unsaved source-only history or metadata may remain unavailable "
                       "from the source; external tool effects are not undone.";
    if (auto error = dialog.begin(renderer, std::move(list))) {
        return lighter::outcome_error(Error::protocol("cannot render fork confirmation: " + std::string(error.message())));
    }
    return {};
}

std::string outcome_label(session::TaskOutcome outcome) {
    switch (outcome) {
        case session::TaskOutcome::COMPLETED: return "completed";
        case session::TaskOutcome::CANCELLED: return "cancelled";
        case session::TaskOutcome::FAILED: return "failed";
        case session::TaskOutcome::INTERRUPTED: return "interrupted";
    }
    return "finished";
}

std::string checkpoint_description(const session::ConversationCheckpoint &checkpoint) {
    auto result = "checkpoint #" + std::to_string(checkpoint.id.entry.value);
    if (checkpoint.task_outcome) result += " · " + outcome_label(*checkpoint.task_outcome);
    if (checkpoint.active) {
        result += " · current append point";
    } else if (checkpoint.on_active_branch) {
        result += " · active ancestor";
    } else {
        result += " · preserved branch";
    }
    result += " · " + std::to_string(checkpoint.branch_leaf_count) + (checkpoint.branch_leaf_count == 1 ? " branch" : " branches");
    for (const auto leaf : checkpoint.branch_leaf_examples) result += " #" + std::to_string(leaf.entry.value);
    if (checkpoint.branch_leaf_count > checkpoint.branch_leaf_examples.size()) {
        result += " +" + std::to_string(checkpoint.branch_leaf_count - checkpoint.branch_leaf_examples.size());
    }
    return result;
}

bool checkpoint_matches(const session::ConversationCheckpoint &checkpoint, std::string_view query) {
    if (query.empty()) return true;
    return ascii_case_insensitive_contains(checkpoint.label, query) ||
           ascii_case_insensitive_contains(std::to_string(checkpoint.id.entry.value), query) ||
           ascii_case_insensitive_contains(checkpoint_description(checkpoint), query);
}

SelectableListPage history_page_impl(const std::vector<session::ConversationCheckpoint> &checkpoints, std::string_view query) {
    SelectableListPage page;
    page.items.reserve(checkpoints.size());
    for (const auto &checkpoint : checkpoints) {
        if (!checkpoint_matches(checkpoint, query)) continue;
        constexpr usize k_max_visible_depth = 8;
        const auto visible_depth = std::min(checkpoint.depth, k_max_visible_depth);
        auto primary = std::string(visible_depth * 2, ' ') + (checkpoint.depth > k_max_visible_depth ? "… " :
                                                              checkpoint.depth == 0                  ? "● " :
                                                                                                       "↳ ");
        primary += bounded_utf8(checkpoint.label, 72);
        auto secondary = checkpoint_description(checkpoint);
        page.items.push_back(
            {.id = std::to_string(checkpoint.id.entry.value), .primary = std::move(primary), .secondary = std::move(secondary)});
    }
    return page;
}

SelectableList history_list(const std::vector<session::ConversationCheckpoint> &checkpoints, const SessionHeader &header) {
    auto page = history_page_impl(checkpoints, {});
    SelectableList list("Conversation history", "No completed conversation checkpoints", std::move(page));
    list.set_contextual_header({.identity = "Conversation History", .session = header, .include_session_title = true});
    list.description = "Inspect safe completed boundaries; selecting one does not change history.";
    list.enable_query("No matching conversation checkpoints");
    return list;
}

Result<session::ConversationCheckpointId> parse_checkpoint_choice(std::string_view choice) {
    u64 value = 0;
    const auto parsed = std::from_chars(choice.data(), choice.data() + choice.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != choice.data() + choice.size() || value == 0) {
        return lighter::outcome_error(Error::protocol("history dialog returned an invalid checkpoint identity"));
    }
    return session::ConversationCheckpointId{session::EntryId{.value = value}};
}

Result<void> open_history_actions(SelectableListDialog &dialog, ConsoleRenderer &renderer, session::ConversationCheckpointId checkpoint) {
    SelectableList list(
        "Checkpoint #" + std::to_string(checkpoint.entry.value), "",
        SelectableListPage{
            .items = {
                {.id = "keep", .primary = "Keep current session"},
                {.id = "checkout", .primary = "Checkout here", .secondary = "preserve descendants; next prompt creates a branch"},
                {.id = "fork", .primary = "Fork from here", .secondary = "create and switch to an independent session"},
            }});
    list.description = "Conversation context changes only; filesystem, process, network, and other tool effects are not undone.";
    if (auto error = dialog.begin(renderer, std::move(list))) {
        return lighter::outcome_error(Error::protocol("cannot render history actions: " + std::string(error.message())));
    }
    return {};
}

lighter::Task<lighter::Error> finish_switch(Agent &agent, ConsoleRenderer &renderer, SelectableListDialog &dialog,
                                            application::SessionSwitch switching, std::string success_status) {
    auto *current_queue = agent.session.persistence_queue();
    switching.flush_current(current_queue);
    if (switching.state() == application::SessionSwitchState::AWAITING_UNSAVED_CONFIRMATION) {
        auto confirmation = open_unsaved_confirmation(dialog, renderer);
        if (!confirmation) co_return renderer.status("Session switch error: " + confirmation.error().message());
        auto decision = co_await dialog.next();
        switching.resolve_unsaved(decision && *decision == "switch" ? application::UnsavedSwitchDecision::ABANDON_UNSAVED_HISTORY :
                                                                      application::UnsavedSwitchDecision::STAY,
                                  current_queue->status());
    }
    if (switching.state() == application::SessionSwitchState::CANCELLED) co_return lighter::Error{};
    const auto abandoned_unsaved_history = switching.abandoned_unsaved_history();
    auto prepared = std::move(switching).take_target();
    if (auto error = renderer.load_transcript(std::move(prepared.transcript))) co_return error;
    agent.replace_session(std::move(prepared.model), std::move(prepared.session));
    renderer.set_session_title(agent.session.metadata.title, agent.session.metadata.preview);
    if (auto error = renderer.render(ModelSelected{.name = agent.model.entry.id, .effort = agent.model.reasoning_effort})) co_return error;
    for (const auto &notice : prepared.notices) {
        if (auto error = renderer.notice(notice)) co_return error;
    }
    if (abandoned_unsaved_history) {
        if (auto error = renderer.notice("[switched sessions; the old unsaved history was abandoned and external tool effects were not "
                                         "undone]\n"))
            co_return error;
    }
    co_return renderer.status(std::move(success_status));
}

lighter::Task<lighter::Error> publish_and_switch_to_fork(Agent &agent, ConsoleRenderer &renderer, SelectableListDialog &dialog,
                                                         application::SessionCoordinator &sessions, application::ForkPlan plan,
                                                         std::string success_status) {
    auto *source_queue = agent.session.persistence_queue();
    bool confirmation_required = source_queue == nullptr;
    if (source_queue) {
        auto flushed = source_queue->flush();
        confirmation_required = !flushed;
    }
    bool source_history_unsaved = false;
    if (confirmation_required) {
        auto confirmation = open_unsaved_fork_confirmation(dialog, renderer);
        if (!confirmation) co_return renderer.notice("[fork error: " + confirmation.error().message() + "]\n");
        auto decision = co_await dialog.next();
        if (!decision || *decision != "fork") co_return lighter::Error{};
        source_history_unsaved = !source_queue || source_queue->status().pending_mutations != 0;
    }

    auto published = sessions.publish_fork(std::move(plan));
    if (!published) co_return renderer.notice("[fork error: " + published.error().message() + "]\n");
    auto prepared = *std::move(published);
    if (auto error = renderer.load_transcript(std::move(prepared.transcript))) co_return error;
    agent.replace_session(std::move(prepared.model), std::move(prepared.session));
    renderer.set_session_title(agent.session.metadata.title, agent.session.metadata.preview);
    if (auto error = renderer.render(ModelSelected{.name = agent.model.entry.id, .effort = agent.model.reasoning_effort})) co_return error;
    for (const auto &notice : prepared.notices) {
        if (auto error = renderer.notice(notice)) co_return error;
    }
    if (source_history_unsaved) {
        if (auto error = renderer.notice("[forked the selected prefix; unsaved source-only history or metadata may remain unavailable "
                                         "from the source session, and external tool effects were not undone]\n"))
            co_return error;
    }
    co_return renderer.status(std::move(success_status));
}

} // namespace

SelectableListPage conversation_history_page(const std::vector<session::ConversationCheckpoint> &checkpoints, std::string_view query) {
    return history_page_impl(checkpoints, query);
}

lighter::Task<lighter::Error> resume_session(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                             SelectableListDialog &dialog) {
    if (!sessions || !renderer.terminal || !agent.session.metadata.workspace) {
        co_return renderer.status("Resume picker is unavailable in this session");
    }
    auto opened = open_session_picker(dialog, renderer, *sessions, agent.session.metadata.workspace->key, "Resume session",
                                      "No resumable sessions in this workspace");
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

    const auto resumed_id = session::to_string(*id).substr(0, 8);
    co_return co_await finish_switch(agent, renderer, dialog, *std::move(switching), "Resumed session " + resumed_id);
}

lighter::Error name_current_session(Agent &agent, ConsoleRenderer &renderer, std::optional<std::string> title) {
    agent.session.set_title(std::move(title));
    auto *queue = agent.session.persistence_queue();
    auto saved = queue ? queue->flush() : Result<void>{};
    renderer.set_session_title(agent.session.metadata.title, agent.session.metadata.preview);
    if (saved && queue && queue->status().catalog_degraded) {
        return renderer.notice("[Session name updated; catalog refresh pending: " + queue->status().catalog_detail + "]\n");
    }
    if (saved) return renderer.status("Session name updated");
    return renderer.notice("[Session name changed in memory; saving failed]\n");
}

lighter::Task<lighter::Error> navigate_conversation(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                                    SelectableListDialog &dialog) {
    if (!sessions || !renderer.terminal) co_return renderer.status("Conversation history is unavailable in this session");
    auto checkpoints = agent.session.conversation_checkpoints();
    if (!checkpoints) co_return renderer.notice("[history error: " + checkpoints.error().message() + "]\n");
    SelectableListDialog::LoadPage load = [checkpoints = *checkpoints](std::string_view query, SelectableListPageLoad,
                                                                       std::optional<std::string_view>) -> Result<SelectableListPage> {
        return conversation_history_page(checkpoints, query);
    };
    if (auto error = dialog.begin(renderer, history_list(*checkpoints, renderer.screen.header), std::move(load))) co_return error;
    auto selected = co_await dialog.next();
    if (!selected) co_return lighter::Error{};
    auto checkpoint = parse_checkpoint_choice(*selected);
    if (!checkpoint) co_return renderer.notice("[history error: " + checkpoint.error().message() + "]\n");
    if (auto actions = open_history_actions(dialog, renderer, *checkpoint); !actions) {
        co_return renderer.notice("[history error: " + actions.error().message() + "]\n");
    }
    auto action = co_await dialog.next();
    if (!action || *action == "keep") co_return lighter::Error{};

    if (*action == "checkout") {
        auto projected = project_transcript_at(agent.session, *checkpoint, *agent.tools);
        if (!projected) co_return renderer.notice("[checkout error: " + projected.error().message() + "]\n");
        auto *queue = agent.session.persistence_queue();
        if (!queue) co_return renderer.notice("[checkout error: session persistence is unavailable]\n");
        const auto former_leaf = agent.session.active_leaf;
        auto restore = [&agent, queue, former_leaf]() -> Result<void> {
            if (!former_leaf) return lighter::outcome_error(Error::protocol("checkout had no former append point"));
            auto restored = agent.session.checkout(session::ConversationCheckpointId{*former_leaf});
            if (!restored) return restored;
            return queue->flush();
        };
        auto checked_out = agent.session.checkout(*checkpoint);
        if (!checked_out) co_return renderer.notice("[checkout error: " + checked_out.error().message() + "]\n");
        auto flushed = queue->flush();
        if (!flushed) {
            auto restored = restore();
            if (!restored) {
                co_return renderer.notice("[checkout error: " + flushed.error().message() +
                                          "; the original cursor was restored in memory but is not saving: " + restored.error().message() +
                                          "]\n");
            }
            co_return renderer.notice("[checkout error: " + flushed.error().message() + "; conversation unchanged]\n");
        }
        if (auto error = renderer.load_transcript(*std::move(projected))) {
            auto restored = restore();
            if (!restored) co_return renderer.notice("[checkout cursor restoration error: " + restored.error().message() + "]\n");
            co_return error;
        }
        co_return renderer.status("Checked out checkpoint #" + std::to_string(checkpoint->entry.value) +
                                  "; the next prompt creates a branch");
    }

    if (*action != "fork") co_return renderer.notice("[history error: unknown checkpoint action]\n");
    auto plan = sessions->prepare_fork(agent.session, *checkpoint);
    if (!plan) co_return renderer.notice("[fork error: " + plan.error().message() + "]\n");
    co_return co_await publish_and_switch_to_fork(agent, renderer, dialog, *sessions, *std::move(plan),
                                                  "Forked from checkpoint #" + std::to_string(checkpoint->entry.value));
}

} // namespace liminal::tui
