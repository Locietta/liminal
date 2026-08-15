#include "catalog_lease.h"

namespace liminal::session::detail {

CatalogLease::~CatalogLease() = default;
CatalogLease::CatalogLease(CatalogLease &&) noexcept = default;
CatalogLease &CatalogLease::operator=(CatalogLease &&) noexcept = default;

} // namespace liminal::session::detail
