#include "compact.h"

namespace liminal::provider {

using lighter::fail;
using lighter::Task;

Task<void, Error> local_compact(ProviderView provider, History &history, std::string_view instructions) {
    // Implemented in the next slice; failing honestly beats pretending.
    (void) provider;
    (void) history;
    (void) instructions;
    co_await fail(Error::config("local compaction is not implemented yet"));
}

} // namespace liminal::provider
