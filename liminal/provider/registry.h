#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>

#include "liminal/error.h"
#include "liminal/model/model.h"
#include "liminal/provider/auth.h"
#include "liminal/provider/common.h"

namespace liminal::provider {

enum struct ApiType {
    OPENAI_RESPONSES,
    ANTHROPIC_MESSAGES,
};

struct Instance {
    std::string id;
    std::string name;
    ApiType api = ApiType::OPENAI_RESPONSES;
    std::string base_url;
    AuthResolver auth;
    std::optional<std::string> models_client_version;
    bool discover_models = false;
    std::vector<model::Entry> models;
};

struct Registry {
    const std::vector<Instance> &entries() const noexcept { return providers; }
    const Instance *find(std::string_view id) const noexcept;

    lighter::Task<std::vector<DiscoveredModel>, Error> discover(const Instance &provider) const;
    Result<model::Choice> make_choice(const model::Entry &entry, std::optional<std::string> effort) const;

    std::vector<Instance> providers;
};

Result<Registry> load_registry(const std::filesystem::path &providers_path, const std::filesystem::path &auth_path);

std::filesystem::path default_providers_file();

} // namespace liminal::provider
