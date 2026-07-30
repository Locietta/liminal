#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/interrupt.h>

#include "liminal/agent/loop.h"
#include "liminal/provider/anthropic.h"
#include "liminal/tools/tools.h"

namespace {

std::string env_or(const char *name, std::string fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

constexpr const char *k_default_model = "claude-sonnet-5";
constexpr liminal::u32 k_max_tokens = 8192;

} // namespace

int main() {
    using namespace liminal;

    auto api_key = env_or("ANTHROPIC_API_KEY", "");
    auto auth_token = env_or("ANTHROPIC_AUTH_TOKEN", "");
    if (api_key.empty() && auth_token.empty()) {
        std::fputs("error: neither ANTHROPIC_API_KEY nor ANTHROPIC_AUTH_TOKEN is set\n", stderr);
        return 1;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0); // text deltas must appear immediately

    lighter::EventLoop loop;

    auto interrupts = lighter::InterruptSource::create(loop);
    if (!interrupts) {
        std::fprintf(stderr, "error: cannot watch signals: %s\n", std::string(interrupts.error().message()).c_str());
        return 1;
    }

    anthropic::Client client({
        .api_key = std::move(api_key),
        .auth_token = std::move(auth_token),
        .base_url = env_or("ANTHROPIC_BASE_URL", "https://api.anthropic.com"),
    });

    std::error_code cwd_error;
    auto cwd = std::filesystem::current_path(cwd_error);
    ToolSet tools(cwd_error ? std::string(".") : cwd.string());

    auto model = env_or("LIMINAL_MODEL", k_default_model);
    Agent agent(client, tools, model, k_max_tokens);

    std::printf("liminal - model: %s (tools run unsandboxed with your privileges)\n", model.c_str());

    auto app = run_repl(agent, *interrupts);
    loop.schedule(app);
    loop.run();

    return static_cast<int>(app.result());
}
