#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/interrupt.h>

#include "liminal/agent/loop.h"
#include "liminal/provider/anthropic.h"
#include "liminal/provider/openai.h"
#include "liminal/tools/tools.h"

namespace {

std::string env_or(const char *name, std::string fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

constexpr const char *k_default_anthropic_model = "claude-sonnet-5";
constexpr const char *k_default_openai_model = "gpt-5.6";
constexpr liminal::u32 k_max_tokens = 8192;

} // namespace

int main() {
    using namespace liminal;

    auto anthropic_api_key = env_or("ANTHROPIC_API_KEY", "");
    auto anthropic_auth_token = env_or("ANTHROPIC_AUTH_TOKEN", "");
    auto openai_api_key = env_or("OPENAI_API_KEY", "");
    auto provider_name = env_or("LIMINAL_PROVIDER", "");
    if (provider_name.empty()) {
        provider_name = !anthropic_api_key.empty() || !anthropic_auth_token.empty() ? "anthropic" : "openai";
    }
    if (provider_name != "anthropic" && provider_name != "openai") {
        std::fprintf(stderr, "error: unsupported LIMINAL_PROVIDER '%s' (expected anthropic or openai)\n", provider_name.c_str());
        return 1;
    }
    if (provider_name == "anthropic" && anthropic_api_key.empty() && anthropic_auth_token.empty()) {
        std::fputs("error: neither ANTHROPIC_API_KEY nor ANTHROPIC_AUTH_TOKEN is set\n", stderr);
        return 1;
    }
    if (provider_name == "openai" && openai_api_key.empty()) {
        std::fputs("error: OPENAI_API_KEY is not set\n", stderr);
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

    if (provider_name == "anthropic") {
        anthropic::Client client({
            .api_key = std::move(anthropic_api_key),
            .auth_token = std::move(anthropic_auth_token),
            .base_url = env_or("ANTHROPIC_BASE_URL", "https://api.anthropic.com"),
        });
        auto model = env_or("LIMINAL_MODEL", k_default_anthropic_model);
        Agent agent(client, tools, model, k_max_tokens);
        std::printf("liminal - provider: anthropic, model: %s (tools run unsandboxed with your privileges)\n", model.c_str());
        auto app = run_repl(agent, *interrupts);
        loop.schedule(app);
        loop.run();
        return static_cast<int>(app.result());
    }

    openai::Client client({
        .api_key = std::move(openai_api_key),
        .organization = env_or("OPENAI_ORGANIZATION", ""),
        .project = env_or("OPENAI_PROJECT", ""),
        .base_url = env_or("OPENAI_BASE_URL", "https://api.openai.com/v1"),
    });
    auto model = env_or("LIMINAL_MODEL", k_default_openai_model);
    Agent agent(client, tools, model, k_max_tokens);
    std::printf("liminal - provider: openai, model: %s (tools run unsandboxed with your privileges)\n", model.c_str());
    auto app = run_repl(agent, *interrupts);
    loop.schedule(app);
    loop.run();
    return static_cast<int>(app.result());
}
