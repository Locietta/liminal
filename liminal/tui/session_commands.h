#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/agent/agent.h>
#include <liminal/application/session_coordinator.h>
#include <liminal/tui/console_renderer.h>
#include <liminal/tui/selectable_list_dialog.h>

namespace liminal::tui {

SelectableListPage conversation_history_page(const std::vector<session::ConversationCheckpoint> &checkpoints, std::string_view query);

lighter::Task<lighter::Error> resume_session(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                             SelectableListDialog &dialog);
lighter::Task<lighter::Error> start_new_session(Agent &agent, application::SessionCoordinator *sessions,
                                                const session::SessionWorkspace &workspace, model::Catalog &models,
                                                ConsoleRenderer &renderer, SelectableListDialog &dialog);
lighter::Task<lighter::Error> navigate_conversation(Agent &agent, application::SessionCoordinator *sessions, ConsoleRenderer &renderer,
                                                    SelectableListDialog &dialog);
lighter::Error name_current_session(Agent &agent, ConsoleRenderer &renderer, std::optional<std::string> title);

} // namespace liminal::tui
