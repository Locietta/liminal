#pragma once

#include <filesystem>
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

/// Runtime model catalog. Bundled/configured entries are authoritative and
/// provider discovery is opt-in and best-effort.
struct Catalog {
    Catalog(std::filesystem::path providers_path, std::filesystem::path auth_path);

    lighter::Task<RefreshResult, Error> refresh();
    Result<Choice> select(std::string_view selector) const;

    const std::vector<Entry> &entries() const noexcept { return models; }
    const std::filesystem::path &providers_file() const noexcept { return providers_path; }

private:
    std::filesystem::path providers_path;
    std::filesystem::path auth_path;
    provider::Registry registry;
    std::vector<Entry> models;
};

} // namespace liminal::model
