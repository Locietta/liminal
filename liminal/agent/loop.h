#pragma once

#include <string>
#include <vector>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/provider/anthropic.h"
#include "liminal/tools/tools.h"

namespace liminal {

struct Agent {
    Agent(anthropic::Client &client, ToolSet &tools, std::string model, u32 max_tokens)
        : client(&client), tools(&tools), model(std::move(model)), max_tokens(max_tokens) {}

    /// One user turn: send prompt, stream the response (text printed through
    /// `callbacks`), execute tool calls until the model stops. History is
    /// transactional - a failed or cancelled turn leaves it untouched.
    lighter::Task<void, Error> run_turn(std::string prompt, const anthropic::StreamCallbacks &callbacks);

    anthropic::Client *client;
    ToolSet *tools;
    std::string model;
    u32 max_tokens;
    std::vector<anthropic::Message> history;
};

/// Interactive stdin/stdout REPL around an Agent. Returns the process exit
/// code. `interrupts` powers Ctrl-C handling: first cancels the in-flight
/// turn, second hard-exits.
lighter::Task<i32> run_repl(Agent &agent, lighter::InterruptSource &interrupts);

} // namespace liminal
