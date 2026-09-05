#pragma once

#include <optional>
#include <string>
#include <vector>

#include <proxy/proxy.h>

#include <lighter/async/runtime/task.h>

#include <liminal/error.h>
#include <liminal/provider/common.h>
#include <liminal/provider/history.h>

namespace liminal::provider {

// Behavior-first provider abstraction: the agent talks to "a source that
// receives history and streams back tokens", never to a concrete vendor.
// Every capability here must be satisfiable by every provider - possibly
// through a generic fallback (e.g. local compaction via complete()) - so
// callers never probe for support.

/// Upper bound on the recent user prompts a compaction carries verbatim
/// when the caller supplies no tighter budget.
inline constexpr usize k_default_retained_prompt_tokens = 20'000;

/// Parameters of one compaction. `retained_prompt_tokens` bounds the recent
/// user prompts kept verbatim ahead of the summary; the agent derives it from
/// the model's input budget so the compacted history leaves room for the
/// summary and the next call. The request is owning and travels by value:
/// compaction is a lazy coroutine, so a borrowed request would outlive its
/// caller's temporaries.
struct CompactRequest {
    std::string instructions;
    usize retained_prompt_tokens = k_default_retained_prompt_tokens;
    /// Best-effort target for the estimated size of the compacted history as
    /// a whole (instruction prefix, retained prompts, summary). Only retention
    /// is shrunk toward it, down to nothing: the instructions are fixed and
    /// the summary is bounded by the provider's output limit, so the result
    /// can still exceed the target, and native compaction ignores it because
    /// its output is opaque. The agent's post-compaction admission check is
    /// what rejects an unusable checkpoint. Absent means only
    /// `retained_prompt_tokens` applies.
    std::optional<usize> compacted_context_target_tokens;
    /// Upper bound on the estimated size of a summarization request
    /// (instruction prefix, rendered conversation, summarization prompt). The
    /// oldest conversation items beyond it are left out before the first
    /// attempt; only a provider rejection drops more. Absent means the whole
    /// conversation is attempted first.
    std::optional<usize> summary_request_tokens;
};

PRO_DEF_MEM_DISPATCH(CompleteDispatch, complete);
PRO_DEF_MEM_DISPATCH(CompactDispatch, compact);

struct ProviderFacade
    : pro::facade_builder //
      ::add_convention<
          CompleteDispatch, lighter::Task<ProviderCallCompletion, Error>(const History &history, const std::vector<ToolDefinition> &tools,
                                                                         const StreamCallbacks &callbacks)
      >::add_convention<CompactDispatch, lighter::Task<void, Error>(History &history, CompactRequest request)>::build {};

using Provider = pro::proxy<ProviderFacade>;
using ProviderView = pro::proxy_view<ProviderFacade>;

} // namespace liminal::provider
