#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <liminal/session/catalog.h>
#include <liminal/session/store_test.h>

namespace liminal::session {

struct CatalogIndexer;

struct SessionRepository::State {
    State(std::filesystem::path root, SessionCatalog catalog) : root(std::move(root)), catalog(std::move(catalog)) {}

    std::filesystem::path root;
    SessionCatalog catalog;
    std::vector<std::string> warnings;
    testing::StorageHook storage_hook;
    std::shared_ptr<CatalogIndexer> indexer;
};

} // namespace liminal::session
