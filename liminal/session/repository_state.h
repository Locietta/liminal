#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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

    void fail_once(testing::StorageFailure replacement) {
        std::scoped_lock lock(mutex);
        failure = replacement;
    }

    bool consume(testing::StorageFailure expected) {
        std::scoped_lock lock(mutex);
        if (failure != expected) return false;
        failure.reset();
        return true;
    }

    mutable std::mutex mutex;
    testing::StorageHook hook;
    std::optional<testing::StorageFailure> failure;
};

struct SessionRepository::State {
    explicit State(std::filesystem::path root) : root(std::move(root)) {}

    std::filesystem::path root;
    std::optional<SessionCatalog> catalog;
    std::optional<Error> catalog_error;
    std::vector<std::string> warnings;
    StorageHookSlot storage_hook;
    std::shared_ptr<CatalogIndexer> indexer;
};

} // namespace liminal::session
