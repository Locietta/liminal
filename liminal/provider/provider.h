#pragma once

#include <string_view>
#include <vector>

#include <proxy/proxy.h>

#include <lighter/async/runtime/task.h>

#include "liminal/error.h"
#include "liminal/provider/common.h"
#include "liminal/provider/history.h"

namespace liminal::provider {

// Behavior-first provider abstraction: the agent talks to "a source that
// receives history and streams back tokens", never to a concrete vendor.
// Every capability here must be satisfiable by every provider - possibly
// through a generic fallback (e.g. local compaction via complete()) - so
// callers never probe for support.

PRO_DEF_MEM_DISPATCH(CompleteDispatch, complete);
PRO_DEF_MEM_DISPATCH(CompactDispatch, compact);

struct ProviderFacade
    : pro::facade_builder //
      ::add_convention<
          CompleteDispatch, lighter::Task<TurnResponse, Error>(const History &history, const std::vector<ToolDefinition> &tools,
                                                               const StreamCallbacks &callbacks)
      >::add_convention<CompactDispatch, lighter::Task<void, Error>(History &history, std::string_view instructions)>::build {};

using Provider = pro::proxy<ProviderFacade>;
using ProviderView = pro::proxy_view<ProviderFacade>;

} // namespace liminal::provider
