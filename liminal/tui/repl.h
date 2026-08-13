#pragma once

#include <string>
#include <vector>

#include <lighter/async/async.h>

#include <liminal/agent/agent.h>
#include <liminal/model/catalog.h>

namespace liminal::tui {

lighter::Task<i32> run_repl(Agent &agent, lighter::InterruptSource &interrupts, model::Catalog &models,
                            std::vector<std::string> startup_notices = {});

} // namespace liminal::tui
