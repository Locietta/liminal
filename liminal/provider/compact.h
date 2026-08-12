#pragma once

#include <string_view>

#include <lighter/async/runtime/task.h>

#include <liminal/error.h>
#include <liminal/provider/provider.h>

namespace liminal::provider {

/// Provider-agnostic compaction: asks the model (through `provider->complete()`)
/// to summarize the transcript, then replaces everything before the last task
/// boundary with the summary. Works against any conforming provider; providers
/// with a native compaction endpoint should try that first and use this as the
/// fallback.
lighter::Task<void, Error> local_compact(ProviderView provider, History &history, std::string_view instructions);

} // namespace liminal::provider
