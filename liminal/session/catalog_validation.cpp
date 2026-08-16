#include "catalog_validation.h"

#include <limits>

#include <lighter/encoding/utf8.h>

namespace liminal::session::detail {

Result<void> validate_catalog_projection(const CatalogProjection &projection) {
    if (projection.observed_revision == 0 || projection.observed_revision > std::numeric_limits<i64>::max()) {
        return lighter::outcome_error(Error::storage("catalog projection has an invalid authoritative revision"));
    }
    if (projection.workspace_key.empty() || projection.summary.updated_at_ms <= 0) {
        return lighter::outcome_error(Error::storage("catalog projection has incomplete discovery metadata"));
    }
    if (projection.summary.title && (projection.summary.title->empty() || projection.summary.title->size() > 200 ||
                                     !lighter::encoding::utf8::is_valid(*projection.summary.title))) {
        return lighter::outcome_error(Error::storage("catalog projection has an invalid title"));
    }
    if (projection.summary.preview.size() > 240 || !lighter::encoding::utf8::is_valid(projection.summary.preview)) {
        return lighter::outcome_error(Error::storage("catalog projection has an invalid preview"));
    }
    return {};
}

Result<void> validate_authoritative_projection(const CatalogProjection &projection, i64 created_at_ms, std::string_view workspace_root) {
    if (auto valid = validate_catalog_projection(projection); !valid) return valid;
    if (created_at_ms <= 0 || projection.summary.updated_at_ms < created_at_ms || workspace_root.empty()) {
        return lighter::outcome_error(Error::storage("authoritative session singleton has invalid discovery metadata"));
    }
    return {};
}

} // namespace liminal::session::detail
