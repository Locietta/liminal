#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>

#include "liminal/error.h"
#include "liminal/model/model.h"
#include "liminal/provider/registry.h"

namespace liminal::model {

struct RefreshResult {
    std::vector<std::string> warnings;
};

/// Injectable boundary for catalog loading and provider model discovery.
struct CatalogSources {
    std::copyable_function<Result<provider::Registry>() const> load;
    std::copyable_function<lighter::Task<std::vector<provider::DiscoveredModel>, Error>(const provider::Registry &registry,
                                                                                        const provider::Instance &provider) const>
        discover;
};

/// Runtime model catalog. Bundled/configured entries are authoritative and
/// provider discovery is opt-in and best-effort.
struct Catalog {
    Catalog(std::filesystem::path providers_path, std::filesystem::path auth_path);
    explicit Catalog(CatalogSources sources);

    lighter::Task<RefreshResult, Error> refresh();
    Result<Choice> select(std::string_view selector) const;

    const std::vector<Entry> &entries() const noexcept { return models; }
    const std::filesystem::path &providers_file() const noexcept { return providers_path; }

private:
    std::filesystem::path providers_path;
    CatalogSources sources;
    provider::Registry registry;
    std::vector<Entry> models;
};

} // namespace liminal::model
