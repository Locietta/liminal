#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <proxy/proxy.h>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/interrupt.h>

#include "liminal/agent/loop.h"
#include "liminal/provider/anthropic.h"
#include "liminal/provider/openai.h"
#include "liminal/tools/tools.h"

namespace {

using namespace liminal;

std::string env_or(const char *name, std::string fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

constexpr const char *k_default_anthropic_model = "claude-sonnet-5";
constexpr const char *k_default_openai_model = "gpt-5.6";
constexpr u32 k_max_tokens = 8192;

Result<ProviderChoice> make_provider(std::string_view name) {
    if (name == "anthropic") {
        auto api_key = env_or("ANTHROPIC_API_KEY", "");
        auto auth_token = env_or("ANTHROPIC_AUTH_TOKEN", "");
        if (api_key.empty() && auth_token.empty()) {
            return lighter::outcome_error(Error::config("neither ANTHROPIC_API_KEY nor ANTHROPIC_AUTH_TOKEN is set"));
        }
        auto model = env_or("LIMINAL_MODEL", k_default_anthropic_model);
        return ProviderChoice{
            .handle = pro::make_proxy<provider::ProviderFacade, anthropic::Client>(anthropic::ClientOptions{
                .api_key = std::move(api_key),
                .auth_token = std::move(auth_token),
                .base_url = env_or("ANTHROPIC_BASE_URL", "https://api.anthropic.com"),
                .model = model,
                .max_tokens = k_max_tokens,
            }),
            .name = "anthropic",
            .model = std::move(model),
        };
    }
    if (name == "openai") {
        auto api_key = env_or("OPENAI_API_KEY", "");
        if (api_key.empty()) {
            return lighter::outcome_error(Error::config("OPENAI_API_KEY is not set"));
        }
        auto model = env_or("LIMINAL_MODEL", k_default_openai_model);
        return ProviderChoice{
            .handle = pro::make_proxy<provider::ProviderFacade, openai::Client>(openai::ClientOptions{
                .api_key = std::move(api_key),
                .organization = env_or("OPENAI_ORGANIZATION", ""),
                .project = env_or("OPENAI_PROJECT", ""),
                .base_url = env_or("OPENAI_BASE_URL", "https://api.openai.com/v1"),
                .model = model,
                .max_output_tokens = k_max_tokens,
            }),
            .name = "openai",
            .model = std::move(model),
        };
    }
    return lighter::outcome_error(Error::config("unknown provider '" + std::string(name) + "' (expected anthropic or openai)"));
}

} // namespace

int main() {
    auto provider_name = env_or("LIMINAL_PROVIDER", "");
    if (provider_name.empty()) {
        bool has_anthropic = !env_or("ANTHROPIC_API_KEY", "").empty() || !env_or("ANTHROPIC_AUTH_TOKEN", "").empty();
        provider_name = has_anthropic ? "anthropic" : "openai";
    }
    auto choice = make_provider(provider_name);
    if (!choice) {
        std::fprintf(stderr, "error: %s\n", choice.error().message().c_str());
        return 1;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0); // text deltas must appear immediately

    lighter::EventLoop loop;

    auto interrupts = lighter::InterruptSource::create(loop);
    if (!interrupts) {
        std::fprintf(stderr, "error: cannot watch signals: %s\n", std::string(interrupts.error().message()).c_str());
        return 1;
    }

    std::error_code cwd_error;
    auto cwd = std::filesystem::current_path(cwd_error);
    ToolSet tools(cwd_error ? std::string(".") : cwd.string());

    Agent agent(*std::move(choice), tools);
    std::printf("liminal - provider: %s, model: %s (tools run unsandboxed with your privileges)\n", agent.provider.name.c_str(),
                agent.provider.model.c_str());

    auto app = run_repl(agent, *interrupts, make_provider);
    loop.schedule(app);
    loop.run();
    return static_cast<int>(app.result());
}
