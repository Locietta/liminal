#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <lighter/async/async.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/interrupt.h>

#include <liminal/agent/agent.h>
#include <liminal/agent/default_instructions.h>
#include <liminal/context/context.h>
#include <liminal/context/project_instructions.h>
#include <liminal/model/catalog.h>
#include <liminal/provider/codex_auth.h>
#include <liminal/provider/registry.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/repl.h>

namespace {

using namespace liminal;

std::string env_or(const char *name, std::string fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

lighter::Task<i32> run_app(ToolSet &tools, lighter::InterruptSource &interrupts, model::Catalog &models) {
    auto refreshed = co_await models.refresh();
    if (!refreshed) {
        std::fprintf(stderr, "error: %s\n", refreshed.error().message().c_str());
        co_return 1;
    }
    for (const auto &warning : refreshed->warnings) {
        std::fprintf(stderr, "model warning: %s\n", warning.c_str());
    }
    if (models.entries().empty()) {
        std::fprintf(stderr, "error: no configured models; run 'liminal login codex' or configure %s\n",
                     models.providers_file().string().c_str());
        co_return 1;
    }

    auto selector = env_or("LIMINAL_MODEL", "");
    if (selector.empty()) {
        const auto &first = models.entries().front();
        selector = first.provider + "/" + first.id;
    }
    auto initial = models.select(selector);
    if (!initial) {
        std::fprintf(stderr, "error: %s\n", initial.error().message().c_str());
        co_return 1;
    }

    std::vector<context::InstructionSource> instructions;
    if (const char *system = std::getenv("LIMINAL_SYSTEM_PROMPT"); system && *system) {
        instructions.push_back({
            .authority = context::InstructionAuthority::RUNTIME,
            .trust = context::InstructionTrust::OPERATOR,
            .origin = "environment:LIMINAL_SYSTEM_PROMPT",
            .content = system,
        });
    } else {
        instructions.push_back(default_runtime_instruction());
    }
    const char *developer = std::getenv("LIMINAL_DEVELOPER_PROMPT");
    if (developer && *developer) {
        instructions.push_back({
            .authority = context::InstructionAuthority::APPLICATION,
            .trust = context::InstructionTrust::OPERATOR,
            .origin = "environment:LIMINAL_DEVELOPER_PROMPT",
            .content = developer,
        });
    } else {
        instructions.push_back(default_application_instruction());
    }
    auto instruction_files = pro::make_proxy<context::InstructionFilesFacade, context::LocalInstructionFiles>();
    auto project_root = context::discover_project_root(tools.working_directory);
    if (!project_root) {
        std::fprintf(stderr, "error: %s\n", project_root.error().message().c_str());
        co_return 1;
    }
    auto project_instructions = context::ProjectInstructionResolver{}.resolve(*project_root, tools.working_directory, instruction_files);
    if (!project_instructions) {
        std::fprintf(stderr, "error: %s\n", project_instructions.error().message().c_str());
        co_return 1;
    }
    for (auto &instruction : *project_instructions) {
        instructions.push_back(std::move(instruction));
    }
    Agent agent(*std::move(initial), tools, std::move(instructions));
    co_return co_await tui::run_repl(agent, interrupts, models);
}

int run_codex_login() {
    lighter::EventLoop loop;
    auto notice = [](std::string_view url, std::string_view code) {
        std::printf("Open %.*s and enter code %.*s\n", static_cast<int>(url.size()), url.data(), static_cast<int>(code.size()),
                    code.data());
    };
    auto login = codex::login_device(codex::default_auth_file(), notice);
    loop.schedule(login);
    loop.run();
    auto result = login.result();
    if (!result) {
        std::fprintf(stderr, "error: %s\n", result.error().message().c_str());
        return 1;
    }
    std::puts("Codex subscription login saved.");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        if (argc == 3 && std::string_view(argv[1]) == "login" && std::string_view(argv[2]) == "codex") {
            return run_codex_login();
        }
        std::fprintf(stderr, "usage: %s [login codex]\n", argv[0]);
        return 2;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0); // text deltas must appear immediately

    std::error_code cwd_error;
    auto cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        std::fprintf(stderr, "error: cannot resolve working directory: %s\n", cwd_error.message().c_str());
        return 1;
    }
    ToolSet tools(std::move(cwd));
    model::Catalog models(provider::default_providers_file(), codex::default_auth_file());

    lighter::EventLoop loop;
    auto interrupts = lighter::InterruptSource::create(loop);
    if (!interrupts) {
        std::fprintf(stderr, "error: cannot watch process controls: %s\n", std::string(interrupts.error().message()).c_str());
        return 1;
    }

    auto app = run_app(tools, *interrupts, models);
    loop.schedule(app);
    loop.run();
    return static_cast<int>(app.result());
}
