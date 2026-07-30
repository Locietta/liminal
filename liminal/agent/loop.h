#pragma once

#include <string>
#include <vector>

#include <lighter/async/async.h>

#include "liminal/error.h"
#include "liminal/provider/common.h"
#include "liminal/tools/tools.h"

namespace liminal {

/// Statically dispatched over a concrete provider client. A client supplies
/// its own History/Response types and the small turn-adaptation operations
/// used in loop.cpp; provider-specific methods remain available on `client`.
template <typename Client>
struct Agent {
    Agent(Client &client, ToolSet &tools, std::string model, u32 max_tokens)
        : client(&client), tools(&tools), model(std::move(model)), max_tokens(max_tokens) {}

    /// One user turn: send prompt, stream the response (text printed through
    /// `callbacks`), execute tool calls until the model stops. History is
    /// transactional - a failed or cancelled turn leaves it untouched.
    lighter::Task<void, Error> run_turn(std::string prompt, const provider::StreamCallbacks &callbacks);

    Client *client;
    ToolSet *tools;
    std::string model;
    u32 max_tokens;
    typename Client::History history;
};

/// Interactive stdin/stdout REPL around an Agent. Returns the process exit
/// code. `interrupts` powers Ctrl-C handling: first cancels the in-flight
/// turn, second hard-exits.
template <typename Client>
lighter::Task<i32> run_repl(Agent<Client> &agent, lighter::InterruptSource &interrupts);

} // namespace liminal
