#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/agent/agent.h>
#include <liminal/tui/console_renderer.h>

namespace liminal::tui {

/// Routes interactive submissions independently of the running agent task.
/// Steering is durable session state; follow-ups are explicit local drafts.
struct SessionInput {
    SessionInput(Agent &agent, ConsoleRenderer &renderer) : agent(&agent), renderer(&renderer) {}

    lighter::Error submit(std::string text);
    lighter::Task<std::string> next();
    lighter::Error refresh();
    void pause_follow_ups();

private:
    struct QueuedPrompt {
        u64 id;
        session::SessionId session_id;
        std::string text;
        bool command = false;
        bool held = false;
    };

    lighter::Error manage_pending(std::string_view arguments);
    Agent *agent;
    ConsoleRenderer *renderer;
    std::vector<QueuedPrompt> queued;
    u64 next_id = 1;
    lighter::Event ready;
};

} // namespace liminal::tui
