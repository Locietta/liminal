#include "catalog_lease.h"
#include "store_test.h"

#include <mutex>

namespace {

std::mutex conflict_hook_mutex;
liminal::session::testing::CatalogInitializationConflictHook conflict_hook;

} // namespace

namespace liminal::session::detail {

CatalogLease::~CatalogLease() = default;
CatalogLease::CatalogLease(CatalogLease &&) noexcept = default;
CatalogLease &CatalogLease::operator=(CatalogLease &&) noexcept = default;

void notify_catalog_initialization_conflict() {
    testing::CatalogInitializationConflictHook hook;
    {
        std::scoped_lock lock(conflict_hook_mutex);
        hook = conflict_hook;
    }
    if (hook) hook();
}

} // namespace liminal::session::detail

namespace liminal::session::testing {

void set_catalog_initialization_conflict_hook(CatalogInitializationConflictHook hook) {
    std::scoped_lock lock(conflict_hook_mutex);
    conflict_hook = std::move(hook);
}

} // namespace liminal::session::testing
