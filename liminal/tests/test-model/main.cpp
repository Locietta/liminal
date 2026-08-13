#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

#include <liminal/model/catalog.h>
#include <liminal/provider/detail/codex_auth_flow.h>

namespace {

using namespace lighter::types;
using namespace liminal;
using namespace std::chrono_literals;
constexpr std::string_view k_access_token =
    "x.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjb3VudC0xMjMifX0.x";

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

void write(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    require(static_cast<bool>(output), "failed to write test providers file");
}

lighter::Outcome<model::RefreshResult, Error, void> refresh(model::Catalog &catalog) {
    lighter::EventLoop loop;
    auto operation = catalog.refresh();
    loop.schedule(operation);
    loop.run();
    return operation.result();
}

void test_catalog_discovery_is_opt_in() {
    model::CatalogSources sources{
        .load = []() -> Result<provider::Registry> {
            provider::Registry registry;
            registry.providers = {
                {
                    .id = "anthropic",
                    .name = "Anthropic",
                    .api = provider::ApiType::ANTHROPIC_MESSAGES,
                    .discover_models = true,
                    .models = {{.provider = "anthropic", .id = "configured", .name = "Configured Name"}},
                },
                {
                    .id = "openai",
                    .name = "OpenAI",
                    .api = provider::ApiType::OPENAI_RESPONSES,
                    .discover_models = false,
                    .models = {{.provider = "openai", .id = "manual", .name = "Manual Model"}},
                },
            };
            return registry;
        },
        .discover = [](const provider::Registry &,
                       const provider::Instance &instance) -> lighter::Task<std::vector<provider::DiscoveredModel>, Error> {
            require(instance.id == "anthropic", "catalog discovered models for a provider without opt-in");
            co_return std::vector<provider::DiscoveredModel>{
                {.id = "configured", .name = "Discovered Override"},
                {.id = "discovered", .name = "Discovered Model"},
            };
        },
    };
    model::Catalog catalog(std::move(sources));

    auto refreshed = refresh(catalog);

    require(refreshed.has_value() && refreshed->warnings.empty(), "mocked catalog discovery failed");
    require(catalog.entries().size() == 3, "catalog did not merge configured and discovered models");
    auto configured = catalog.select("anthropic/configured");
    require(configured && configured->entry.name == "Configured Name", "discovery replaced configured model metadata");
    require(catalog.select("openai/manual").has_value(), "non-discovered manual model was lost");
}

void test_catalog_discovery_failure_preserves_manual_models() {
    model::CatalogSources sources{
        .load = []() -> Result<provider::Registry> {
            provider::Registry registry;
            registry.providers = {{
                .id = "gateway",
                .name = "Gateway",
                .api = provider::ApiType::OPENAI_RESPONSES,
                .discover_models = true,
                .models = {{.provider = "gateway", .id = "manual", .reasoning_efforts = {"medium"}}},
            }};
            return registry;
        },
        .discover = [](const provider::Registry &,
                       const provider::Instance &) -> lighter::Task<std::vector<provider::DiscoveredModel>, Error> {
            co_await lighter::fail(Error::http_status(404, "not_found", "models endpoint missing", {}));
        },
    };
    model::Catalog catalog(std::move(sources));

    auto refreshed = refresh(catalog);

    require(refreshed && refreshed->warnings.size() == 1, "discovery failure did not become one catalog warning");
    require(catalog.select("manual@medium").has_value(), "discovery failure removed the configured manual model");
}

void test_device_login_flow_is_scriptable() {
    usize poll_count = 0;
    usize sleep_count = 0;
    codex::detail::DeviceLoginAttempts attempts{
        .start = []() -> lighter::Task<codex::detail::DeviceStart, Error> {
            co_return codex::detail::DeviceStart{
                .verification_url = "https://auth.example/device",
                .device_auth_id = "device-123",
                .user_code = "TEST-CODE",
                .interval = 1s,
            };
        },
        .poll = [&poll_count](const codex::detail::DeviceStart &device) -> lighter::Task<codex::detail::DeviceToken, Error> {
            ++poll_count;
            if (poll_count == 1) co_await lighter::fail(Error::http_status(400, {}, "slow_down", {}));
            if (poll_count == 2) co_await lighter::fail(Error::http_status(403, {}, "authorization pending", {}));
            require(device.interval == 6s, "device polling did not retain the slow-down interval");
            co_return codex::detail::DeviceToken{.authorization_code = "authorization-code", .code_verifier = "code-verifier"};
        },
        .exchange = [](const codex::detail::DeviceToken &token) -> lighter::Task<codex::detail::TokenResponse, Error> {
            require(token.authorization_code == "authorization-code" && token.code_verifier == "code-verifier",
                    "device login exchanged the wrong authorization grant");
            co_return codex::detail::TokenResponse{
                .access_token = std::string(k_access_token),
                .refresh_token = "refresh-token",
                .expires_in = 3600,
            };
        },
        .sleep = [&sleep_count](std::chrono::seconds delay) -> lighter::Task<> {
            constexpr std::array expected_delays{1s, 6s, 6s};
            require(sleep_count < expected_delays.size() && delay == expected_delays[sleep_count],
                    "device login used the wrong poll interval");
            ++sleep_count;
            co_return;
        },
        .save = [](const codex::detail::Credentials &credentials) -> Result<void> {
            require(credentials.account_id == "account-123" && credentials.refresh_token == "refresh-token",
                    "device login saved incomplete credentials");
            require(credentials.expires_at == 3'601'000, "device login calculated the wrong expiry");
            return {};
        },
        .now = [] { return std::chrono::steady_clock::time_point{}; },
        .now_unix_milliseconds = [] { return 1000; },
    };
    bool notice_seen = false;
    auto task = codex::detail::login_device(
        attempts,
        [&](std::string_view url, std::string_view code) { notice_seen = url == "https://auth.example/device" && code == "TEST-CODE"; },
        15min);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();

    require(task.result().has_value() && notice_seen && poll_count == 3 && sleep_count == 3, "scripted device login did not complete");
}

void test_expired_codex_auth_refreshes_and_resolves_headers() {
    codex::detail::RefreshAttempts attempts{
        .refresh = [](const std::string &refresh_token) -> lighter::Task<codex::detail::TokenResponse, Error> {
            require(refresh_token == "old-refresh", "Codex refresh used the wrong token");
            co_return codex::detail::TokenResponse{
                .access_token = std::string(k_access_token),
                .refresh_token = "new-refresh",
                .expires_in = 3600,
            };
        },
        .save = [](const codex::detail::Credentials &credentials) -> Result<void> {
            require(credentials.refresh_token == "new-refresh" && credentials.account_id == "account-123",
                    "refreshed credentials were not persisted");
            return {};
        },
        .now_unix_milliseconds = [] { return 10'000; },
    };
    codex::detail::Credentials credentials{
        .access_token = "expired",
        .refresh_token = "old-refresh",
        .expires_at = 0,
        .account_id = "old-account",
    };
    auto task = codex::detail::resolve_auth(attempts, credentials, 1min);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();
    auto resolved = task.result();

    require(resolved && resolved->bearer_token == k_access_token, "Codex resolver did not use the refreshed access token");
    require(resolved->headers.size() == 3 && resolved->headers[0].value == "account-123",
            "Codex resolver did not apply subscription headers");
}

void test_codex_auth_load_is_strict() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-auth-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    const auto path = directory / "auth.json";

    auto auth = codex::load_auth(path);
    require(!auth && auth.error().code == ErrorCode::AUTH_NOT_CONFIGURED,
            "a missing auth file must report that Codex auth is not configured");

    write(path, R"({})");
    auth = codex::load_auth(path);
    require(!auth && auth.error().code == ErrorCode::AUTH_NOT_CONFIGURED,
            "an auth file without Codex credentials must report that auth is not configured");

    write(path, R"({)");
    auth = codex::load_auth(path);
    require(!auth && auth.error().code == ErrorCode::AUTH_INVALID, "malformed auth JSON must report invalid auth");

    write(path, R"({"codex":{}})");
    auth = codex::load_auth(path);
    require(!auth && auth.error().code == ErrorCode::AUTH_INVALID, "incomplete Codex credentials must report invalid auth");

    write(
        path,
        R"({"codex":{"type":"oauth","access_token":"access","refresh_token":"refresh","expires_at":4102444800000,"account_id":"account"}})");
    auth = codex::load_auth(path);
    require(auth.has_value(), "complete Codex credentials must produce an auth resolver");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
}

i32 run_all() {
    test_catalog_discovery_is_opt_in();
    test_catalog_discovery_failure_preserves_manual_models();
    test_device_login_flow_is_scriptable();
    test_expired_codex_auth_refreshes_and_resolves_headers();
    test_codex_auth_load_is_strict();

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-models-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    const auto providers = directory / "providers.json";
    const auto auth = directory / "auth.json";
    write(providers, R"({
        "providers": {
            "anthropic-local": {
                "name": "Anthropic Local",
                "api": "anthropic-messages",
                "base_url": "http://localhost:9001",
                "api_key": "anthropic-key",
                "models": [
                    {"id": "shared", "name": "Anthropic Shared"}
                ]
            },
            "openai-local": {
                "name": "OpenAI Local",
                "api": "openai-responses",
                "base_url": "http://localhost:9002/v1",
                "api_key": "openai-key",
                "discover_models": false,
                "models": [
                    {
                        "id": "shared",
                        "name": "Configured Shared",
                        "context_window": 128000,
                        "max_output_tokens": 4096,
                        "context_safety_margin_tokens": 2000,
                        "reasoning_efforts": ["low", "high"],
                        "default_reasoning_effort": "low"
                    },
                    {
                        "id": "manual",
                        "reasoning_efforts": ["medium"],
                        "default_reasoning_effort": "medium"
                    }
                ]
            }
        }
    })");

    model::Catalog catalog(providers, auth);
    auto refreshed = refresh(catalog);
    require(refreshed.has_value(), "catalog refresh failed");
    require(refreshed->warnings.empty(), "catalog refresh produced a warning");
    require(catalog.entries().size() == 3, "catalog produced the wrong entry count");

    require(!catalog.select("shared"), "ambiguous bare model id must fail");
    auto selected = catalog.select("openai-local/shared@high");
    require(selected.has_value(), "qualified configured model did not resolve");
    require(selected->entry.name == "Configured Shared", "configured model metadata was not preserved");
    require(selected->entry.context_window == 128'000 && selected->entry.max_output_tokens == 4096 &&
                selected->entry.context_safety_margin_tokens == 2000,
            "configured context capabilities were not preserved");
    require(selected->reasoning_effort == "high", "explicit effort was not selected");

    selected = catalog.select("manual");
    require(selected.has_value(), "manual model did not resolve");
    require(selected->reasoning_effort == "medium", "default effort was not selected");
    require(!catalog.select("manual@high"), "unsupported effort must fail");

    write(
        providers,
        R"({"providers":{"broken":{"api":"openai-responses","base_url":"x","api_key":"x","models":[{"id":"bad","context_window":4096,"max_output_tokens":4096}]}}})");
    auto invalid_limits = refresh(catalog);
    require(invalid_limits.has_error() && invalid_limits.error().detail.contains("context_window"),
            "invalid context capabilities must fail refresh");
    require(catalog.entries().size() == 3, "failed capability refresh must preserve the previous catalog");

    write(providers, R"({"providers":{"broken":{"api":"wrong","base_url":"x","api_key":"x"}}})");
    auto invalid = refresh(catalog);
    require(invalid.has_error(), "invalid provider API must fail refresh");
    require(catalog.entries().size() == 3, "failed refresh must preserve the previous catalog");

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
