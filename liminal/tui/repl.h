#pragma once

#include <lighter/async/async.h>

#include <liminal/agent/agent.h>
#include <liminal/model/catalog.h>

namespace liminal::tui {

lighter::Task<i32> run_repl(Agent &agent, lighter::InterruptSource &interrupts, model::Catalog &models);

} // namespace liminal::tui
