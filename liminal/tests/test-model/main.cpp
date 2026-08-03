#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/types.hpp>

#include <liminal/model/catalog.h>

namespace {

using namespace lighter::types;
using namespace liminal;

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

i32 run_all() {
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
    require(selected->reasoning_effort == "high", "explicit effort was not selected");

    selected = catalog.select("manual");
    require(selected.has_value(), "manual model did not resolve");
    require(selected->reasoning_effort == "medium", "default effort was not selected");
    require(!catalog.select("manual@high"), "unsupported effort must fail");

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
