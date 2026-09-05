#pragma once

#include <deque>
#include <string>
#include <string_view>

#include <lighter/async/async.h>

#include <liminal/agent/agent.h>
#include <liminal/tui/console_renderer.h>

namespace liminal::tui {

/// Routes interactive submissions independently of the running agent task.
/// Busy prompts become durable steering; commands and idle prompts go to the REPL.
struct SessionInput {
    SessionInput(Agent &agent, ConsoleRenderer &renderer) : agent(&agent), renderer(&renderer) {}

    lighter::Error submit(std::string text);
    lighter::Task<std::string> next();
    lighter::Error refresh();

private:
    lighter::Error manage_pending(std::string_view arguments);
    Agent *agent;
    ConsoleRenderer *renderer;
    std::deque<std::string> submissions;
    lighter::Event ready;
};

} // namespace liminal::tui
