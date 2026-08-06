#include "registry.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/json.hpp>
#include <proxy/proxy.h>

#include <lighter/async/async.h>
#include <lighter/codec/json/json.h>

#include <liminal/provider/anthropic.h>
#include <liminal/provider/codex_auth.h>
#include <liminal/provider/openai.h>
#include <liminal/provider/provider.h>

namespace liminal::provider {

namespace {

struct ConfigModel {
    std::string id;
    std::optional<std::string> name;
    std::optional<u32> context_window;
    std::optional<u32> max_output_tokens;
    std::optional<u32> context_safety_margin_tokens;
    std::optional<std::vector<std::string>> reasoning_efforts;
    std::optional<std::string> default_reasoning_effort;
};

struct ConfigProvider {
    std::optional<std::string> name;
    std::string api;
    std::string base_url;
    std::string api_key;
    bool discover_models = false;
    std::vector<ConfigModel> models;
};

struct Config {
    std::map<std::string, ConfigProvider> providers;
};

Result<Config> load_config(const std::filesystem::path &path) {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return lighter::outcome_error(Error::config("cannot inspect providers file '" + path.string() + "': " + exists_error.message()));
    }
    if (!exists) return Config{};

    std::ifstream input(path, std::ios::binary);
    if (!input) return lighter::outcome_error(Error::config("cannot open providers file '" + path.string() + "'"));
    std::string text(std::istreambuf_iterator<char>(input), {});
    Config config;
    constexpr lighter::codec::json::Opts options{{.null_terminated = false, .error_on_unknown_keys = true}};
    if (auto context = glz::read<options>(config, text)) {
        return lighter::outcome_error(Error::config("invalid providers file '" + path.string() + "': " + glz::format_error(context, text)));
    }
    return config;
}

Result<ApiType> parse_api(std::string_view value, std::string_view provider_id) {
    if (value == "openai-responses") return ApiType::OPENAI_RESPONSES;
    if (value == "anthropic-messages") return ApiType::ANTHROPIC_MESSAGES;
    return lighter::outcome_error(
        Error::config("provider '" + std::string(provider_id) + "' api must be 'openai-responses' or 'anthropic-messages'"));
}

bool contains(const std::vector<std::string> &values, std::string_view value) { return std::ranges::find(values, value) != values.end(); }

Result<void> validate_model(const ConfigModel &model, std::string_view provider_id) {
    if (model.id.empty()) {
        return lighter::outcome_error(Error::config("provider '" + std::string(provider_id) + "' has a model with an empty id"));
    }
    if (model.reasoning_efforts) {
        for (const auto &effort : *model.reasoning_efforts) {
            if (effort.empty() || effort == "off") {
                return lighter::outcome_error(
                    Error::config("model '" + std::string(provider_id) + "/" + model.id + "' has an invalid reasoning effort"));
            }
        }
    }
    if (model.default_reasoning_effort &&
        (!model.reasoning_efforts || !contains(*model.reasoning_efforts, *model.default_reasoning_effort))) {
        return lighter::outcome_error(Error::config("model '" + std::string(provider_id) + "/" + model.id +
                                                    "' default_reasoning_effort is not in reasoning_efforts"));
    }
    const auto max_output_tokens = model.max_output_tokens.value_or(model::k_default_max_output_tokens);
    const auto safety_margin = model.context_safety_margin_tokens.value_or(0);
    if (max_output_tokens == 0) {
        return lighter::outcome_error(
            Error::config("model '" + std::string(provider_id) + "/" + model.id + "' max_output_tokens must be positive"));
    }
    if (model.context_window &&
        (*model.context_window == 0 || static_cast<u64>(max_output_tokens) + safety_margin >= *model.context_window)) {
        return lighter::outcome_error(Error::config("model '" + std::string(provider_id) + "/" + model.id +
                                                    "' context_window must exceed max_output_tokens plus its safety margin"));
    }
    return {};
}

Result<void> apply_models(Instance &instance, const std::vector<ConfigModel> &configured) {
    std::vector<std::string> configured_ids;
    configured_ids.reserve(configured.size());
    for (const auto &model : configured) {
        if (auto valid = validate_model(model, instance.id); !valid) return lighter::outcome_error(std::move(valid).error());
        if (std::ranges::contains(configured_ids, model.id)) {
            return lighter::outcome_error(
                Error::config("provider '" + instance.id + "' configures model '" + model.id + "' more than once"));
        }
        configured_ids.push_back(model.id);

        auto found = std::ranges::find(instance.models, model.id, &model::Entry::id);
        if (found == instance.models.end()) {
            instance.models.push_back({.provider = instance.id, .id = model.id, .name = model.name.value_or(model.id)});
            found = std::prev(instance.models.end());
        }
        if (model.name) found->name = *model.name;
        if (model.context_window) found->context_window = model.context_window;
        if (model.max_output_tokens) found->max_output_tokens = *model.max_output_tokens;
        if (model.context_safety_margin_tokens) found->context_safety_margin_tokens = *model.context_safety_margin_tokens;
        if (model.reasoning_efforts) found->reasoning_efforts = *model.reasoning_efforts;
        if (model.default_reasoning_effort) found->default_reasoning_effort = model.default_reasoning_effort;
        if (found->default_reasoning_effort && !contains(found->reasoning_efforts, *found->default_reasoning_effort)) {
            return lighter::outcome_error(Error::config("model '" + instance.id + "/" + model.id +
                                                        "' default_reasoning_effort is not in reasoning_efforts after merging"));
        }
        if (found->context_window &&
            static_cast<u64>(found->max_output_tokens) + found->context_safety_margin_tokens >= *found->context_window) {
            return lighter::outcome_error(Error::config("model '" + instance.id + "/" + model.id +
                                                        "' merged context_window must exceed max_output_tokens plus its safety margin"));
        }
    }
    return {};
}

Result<std::string> resolve_api_key(std::string value, std::string_view provider_id) {
    if (!value.starts_with('$')) return value;
    if (value.size() == 1) {
        return lighter::outcome_error(Error::config("provider '" + std::string(provider_id) + "' has an empty API-key variable"));
    }
    const auto name = value.substr(1);
    const char *resolved = std::getenv(name.c_str());
    if (!resolved || !*resolved) {
        return lighter::outcome_error(Error::config("provider '" + std::string(provider_id) + "' requires environment variable " + name));
    }
    return std::string(resolved);
}

std::string codex_api_base_url() {
    if (const char *override_url = std::getenv("LIMINAL_CODEX_API_BASE_URL"); override_url && *override_url) {
        return override_url;
    }
    return "https://chatgpt.com/backend-api/codex";
}

std::vector<model::Entry> bundled_codex_models() {
    const std::vector<std::string> efforts = {"low", "medium", "high", "xhigh", "max", "ultra"};
    constexpr u32 k_context_window = 272'000;
    constexpr u32 k_context_safety_margin = 13'600;
    return {
        {.provider = "codex",
         .id = "gpt-5.6-sol",
         .name = "GPT-5.6-Sol",
         .context_window = k_context_window,
         .context_safety_margin_tokens = k_context_safety_margin,
         .reasoning_efforts = efforts,
         .default_reasoning_effort = "low"},
        {.provider = "codex",
         .id = "gpt-5.6-terra",
         .name = "GPT-5.6-Terra",
         .context_window = k_context_window,
         .context_safety_margin_tokens = k_context_safety_margin,
         .reasoning_efforts = efforts,
         .default_reasoning_effort = "medium"},
        {.provider = "codex",
         .id = "gpt-5.6-luna",
         .name = "GPT-5.6-Luna",
         .context_window = k_context_window,
         .context_safety_margin_tokens = k_context_safety_margin,
         .reasoning_efforts = efforts,
         .default_reasoning_effort = "high"},
        {.provider = "codex",
         .id = "gpt-5.5",
         .name = "GPT-5.5",
         .context_window = k_context_window,
         .context_safety_margin_tokens = k_context_safety_margin,
         .reasoning_efforts = efforts},
    };
}

} // namespace

const Instance *Registry::find(std::string_view id) const noexcept {
    auto found = std::ranges::find(providers, id, &Instance::id);
    return found == providers.end() ? nullptr : &*found;
}

lighter::Task<std::vector<DiscoveredModel>, Error> Registry::discover(const Instance &provider) const {
    if (provider.api == ApiType::OPENAI_RESPONSES) {
        co_return co_await openai::list_models({
                                                   .auth = provider.auth,
                                                   .base_url = provider.base_url,
                                                   .models_client_version = provider.models_client_version,
                                               })
            .or_fail();
    }
    co_return co_await anthropic::list_models({.auth = provider.auth, .base_url = provider.base_url}).or_fail();
}

Result<model::Choice> Registry::make_choice(const model::Entry &entry, std::optional<std::string> effort) const {
    const auto *provider = find(entry.provider);
    if (!provider) {
        return lighter::outcome_error(Error::config("model provider '" + entry.provider + "' is not configured"));
    }
    model::Choice choice{.entry = entry, .reasoning_effort = std::move(effort)};
    if (provider->api == ApiType::OPENAI_RESPONSES) {
        choice.handle = pro::make_proxy<ProviderFacade, openai::Client>(openai::ClientOptions{
            .auth = provider->auth,
            .base_url = provider->base_url,
            .model = entry.id,
            .reasoning_effort = choice.reasoning_effort,
            .max_output_tokens = provider->codex_subscription ? std::nullopt : std::optional<u32>(entry.max_output_tokens),
            .allow_missing_event_stream_content_type = provider->codex_subscription,
        });
    } else {
        choice.handle = pro::make_proxy<ProviderFacade, anthropic::Client>(anthropic::ClientOptions{
            .auth = provider->auth,
            .base_url = provider->base_url,
            .model = entry.id,
            .reasoning_effort = choice.reasoning_effort,
            .max_tokens = entry.max_output_tokens,
        });
    }
    return choice;
}

Result<Registry> load_registry(const std::filesystem::path &providers_path, const std::filesystem::path &auth_path) {
    auto config = load_config(providers_path);
    if (!config) return lighter::outcome_error(std::move(config).error());

    Registry registry;
    auto codex_auth = codex::load_auth(auth_path);
    if (!codex_auth) return lighter::outcome_error(std::move(codex_auth).error());
    const auto codex_override = config->providers.find("codex");
    if (codex_override != config->providers.end()) {
        const auto &override = codex_override->second;
        if (!override.api.empty() || !override.base_url.empty() || !override.api_key.empty()) {
            return lighter::outcome_error(
                Error::config("built-in provider 'codex' only accepts 'name', 'discover_models', and 'models' overrides"));
        }
    }
    if (*codex_auth) {
        registry.providers.push_back({
            .id = "codex",
            .name = "OpenAI Codex",
            .api = ApiType::OPENAI_RESPONSES,
            .base_url = codex_api_base_url(),
            .auth = **std::move(codex_auth),
            .codex_subscription = true,
            .models_client_version = "0.1.0",
            .models = bundled_codex_models(),
        });
        if (codex_override != config->providers.end()) {
            const auto &override = codex_override->second;
            if (override.name) registry.providers.back().name = *override.name;
            registry.providers.back().discover_models = override.discover_models;
            if (auto applied = apply_models(registry.providers.back(), override.models); !applied) {
                return lighter::outcome_error(std::move(applied).error());
            }
        }
    }

    for (auto &[id, configured] : config->providers) {
        if (id.empty()) return lighter::outcome_error(Error::config("provider IDs must not be empty"));
        if (id == "codex") continue;
        if (configured.base_url.empty()) {
            return lighter::outcome_error(Error::config("provider '" + id + "' requires a non-empty base_url"));
        }
        auto api = parse_api(configured.api, id);
        if (!api) return lighter::outcome_error(std::move(api).error());
        auto key = resolve_api_key(std::move(configured.api_key), id);
        if (!key) return lighter::outcome_error(std::move(key).error());

        Instance instance{
            .id = id,
            .name = configured.name.value_or(id),
            .api = *api,
            .base_url = std::move(configured.base_url),
            .auth = api_key_auth(*std::move(key), *api == ApiType::OPENAI_RESPONSES),
            .discover_models = configured.discover_models,
        };
        instance.models.reserve(configured.models.size());
        if (auto applied = apply_models(instance, configured.models); !applied) {
            return lighter::outcome_error(std::move(applied).error());
        }
        registry.providers.push_back(std::move(instance));
    }
    return registry;
}

std::filesystem::path default_providers_file() {
    if (const char *override_path = std::getenv("LIMINAL_PROVIDERS_FILE"); override_path && *override_path) {
        return override_path;
    }
#ifdef _WIN32
    if (const char *app_data = std::getenv("APPDATA"); app_data && *app_data) {
        return std::filesystem::path(app_data) / "liminal" / "providers.json";
    }
#else
    if (const char *config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home) {
        return std::filesystem::path(config_home) / "liminal" / "providers.json";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "liminal" / "providers.json";
    }
#endif
    return std::filesystem::path("providers.json");
}

} // namespace liminal::provider
