#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <liminal/session/catalog.h>
#include <liminal/session/store_test.h>

namespace liminal::session {

struct CatalogIndexer;

struct StorageHookSlot {
    void notify(testing::StorageEvent event) const {
        testing::StorageHook current;
        {
            std::scoped_lock lock(mutex);
            current = hook;
        }
        if (current) current(event);
    }

    void set(testing::StorageHook replacement) {
        std::scoped_lock lock(mutex);
        hook = std::move(replacement);
    }

    mutable std::mutex mutex;
    testing::StorageHook hook;
};

struct SessionRepository::State {
    State(std::filesystem::path root, SessionCatalog catalog) : root(std::move(root)), catalog(std::move(catalog)) {}

    std::filesystem::path root;
    SessionCatalog catalog;
    std::vector<std::string> warnings;
    StorageHookSlot storage_hook;
    std::shared_ptr<CatalogIndexer> indexer;
};

} // namespace liminal::session
