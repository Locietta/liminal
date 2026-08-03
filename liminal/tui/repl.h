#pragma once

#include <functional>
#include <string_view>

#include <lighter/async/async.h>

#include "liminal/agent/agent.h"

namespace liminal::tui {

using ProviderFactory = std::function<Result<ProviderChoice>(std::string_view name)>;

lighter::Task<i32> run_repl(Agent &agent, lighter::InterruptSource &interrupts, const ProviderFactory &factory);

} // namespace liminal::tui
