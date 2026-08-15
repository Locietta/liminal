#pragma once

#include <functional>

namespace liminal::session::testing {

enum struct StorageEvent {
    AUTHORITATIVE_COMMIT_COMPLETED,
    CATALOG_INDEXER_BEFORE_REFRESH,
    CATALOG_INDEXER_AFTER_REFRESH,
};

using StorageHook = std::copyable_function<void(StorageEvent) const>;
void set_storage_hook(StorageHook hook);

} // namespace liminal::session::testing
