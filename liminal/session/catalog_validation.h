#pragma once

#include <string_view>

#include <liminal/error.h>
#include <liminal/session/catalog.h>

namespace liminal::session::detail {

Result<void> validate_catalog_projection(const CatalogProjection &projection);
Result<void> validate_authoritative_projection(const CatalogProjection &projection, i64 created_at_ms, std::string_view workspace_root);

} // namespace liminal::session::detail
