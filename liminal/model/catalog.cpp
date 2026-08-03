#include "catalog.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/async/async.h>

namespace liminal::model {

namespace {

struct ParsedSelector {
    std::string_view model;
    std::optional<std::string> effort;
};

bool contains(const std::vector<std::string> &values, std::string_view value) { return std::ranges::find(values, value) != values.end(); }

void upsert(std::vector<Entry> &models, Entry entry) {
    auto found = std::ranges::find_if(
        models, [&](const Entry &candidate) { return candidate.provider == entry.provider && candidate.id == entry.id; });
    if (found == models.end()) {
        if (entry.name.empty()) entry.name = entry.id;
        models.push_back(std::move(entry));
        return;
    }
    *found = std::move(entry);
}

Result<ParsedSelector> parse_selector(std::string_view selector) {
    if (selector.empty()) return lighter::outcome_error(Error::config("model selector is empty"));
    ParsedSelector parsed{.model = selector};
    if (const auto separator = selector.rfind('@'); separator != std::string_view::npos) {
        parsed.model = selector.substr(0, separator);
        const auto effort = selector.substr(separator + 1);
        if (parsed.model.empty() || effort.empty()) {
            return lighter::outcome_error(Error::config("expected <model> or <model>@<effort>"));
        }
        parsed.effort = std::string(effort);
    }
    return parsed;
}

} // namespace

Catalog::Catalog(std::filesystem::path providers_path, std::filesystem::path auth_path)
    : providers_path(std::move(providers_path)), auth_path(std::move(auth_path)) {}

lighter::Task<RefreshResult, Error> Catalog::refresh() {
    auto next_registry = provider::load_registry(providers_path, auth_path);
    if (!next_registry) co_await lighter::fail(std::move(next_registry).error());

    std::vector<Entry> refreshed;
    RefreshResult result;
    for (const auto &provider : next_registry->entries()) {
        for (const auto &entry : provider.models) upsert(refreshed, entry);
        if (!provider.discover_models) continue;

        auto discovered = co_await next_registry->discover(provider);
        if (!discovered) {
            result.warnings.push_back(provider.id + ": " + discovered.error().message());
            continue;
        }
        for (auto &entry : *discovered) {
            const auto already_configured = std::ranges::find_if(
                refreshed, [&](const Entry &candidate) { return candidate.provider == provider.id && candidate.id == entry.id; });
            if (already_configured == refreshed.end()) {
                upsert(refreshed, {.provider = provider.id, .id = std::move(entry.id), .name = std::move(entry.name)});
            }
        }
    }

    std::ranges::sort(refreshed, {}, [](const Entry &entry) { return std::pair(entry.provider, entry.id); });
    registry = *std::move(next_registry);
    models = std::move(refreshed);
    co_return result;
}

Result<Choice> Catalog::select(std::string_view selector) const {
    auto parsed = parse_selector(selector);
    if (!parsed) return lighter::outcome_error(std::move(parsed).error());

    std::vector<const Entry *> matches;
    for (const auto &entry : models) {
        if (entry.provider + "/" + entry.id == parsed->model) {
            matches = {&entry};
            break;
        }
        if (entry.id == parsed->model) matches.push_back(&entry);
    }
    if (matches.empty()) {
        return lighter::outcome_error(Error::config("unknown model '" + std::string(parsed->model) + "'"));
    }
    if (matches.size() != 1) {
        return lighter::outcome_error(Error::config("ambiguous model '" + std::string(parsed->model) + "'; use <provider>/<model>"));
    }

    const auto &entry = *matches.front();
    auto effort = parsed->effort ? parsed->effort : entry.default_reasoning_effort;
    if (effort && *effort == "off") {
        effort.reset();
    } else if (effort && !contains(entry.reasoning_efforts, *effort)) {
        return lighter::outcome_error(
            Error::config("model '" + entry.provider + "/" + entry.id + "' does not support effort '" + *effort + "'"));
    }
    return registry.make_choice(entry, std::move(effort));
}

} // namespace liminal::model
