#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <liminal/session/catalog.h>

namespace liminal::session {

struct CatalogIndexer;

struct SessionRepository::State {
    State(std::filesystem::path root, SessionCatalog catalog) : root(std::move(root)), catalog(std::move(catalog)) {}

    std::filesystem::path root;
    SessionCatalog catalog;
    std::vector<std::string> warnings;
    std::shared_ptr<CatalogIndexer> indexer;
};

} // namespace liminal::session
