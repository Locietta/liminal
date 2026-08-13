#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include <liminal/session/paths.h>
#include <liminal/session/persistence.h>
#include <liminal/session/recovery.h>
#include <liminal/session/store.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/repl.h>

namespace {

using namespace liminal;

std::string env_or(const char *name, std::string fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

enum struct StartupKind {
    NEW,
    RESUME,
    CONTINUE,
};

struct StartupOptions {
    StartupKind kind = StartupKind::NEW;
    std::string session;
};

struct SessionSelection {
    session::Session session;
    std::vector<std::string> notices;
};

Result<SessionSelection> select_session(const StartupOptions &options, const std::filesystem::path &working_directory,
                                        const session::WorkspaceIdentity &workspace) {
    auto path = session::state_database_path();
    if (!path) {
        if (options.kind != StartupKind::NEW) return lighter::outcome_error(std::move(path).error());
        SessionSelection selection;
        selection.session.metadata.workspace = session::SessionWorkspace{.root = workspace.root, .key = workspace.key};
        selection.session.metadata.working_directory = working_directory.generic_string();
        auto attached =
            selection.session.attach_persistence(session::PersistenceQueue::create_resolving(selection.session.id, path.error().message()));
        if (!attached) return lighter::outcome_error(std::move(attached).error());
        selection.notices.push_back("[session not saving: " + path.error().message() + "]\n");
        return selection;
    }

    auto opened = session::Store::open(*path);
    if (!opened) {
        if (options.kind != StartupKind::NEW) return lighter::outcome_error(std::move(opened).error());
        SessionSelection selection;
        selection.session.metadata.workspace = session::SessionWorkspace{.root = workspace.root, .key = workspace.key};
        selection.session.metadata.working_directory = working_directory.generic_string();
        auto queue = session::PersistenceQueue::create_reopening(*path, selection.session.id, opened.error().message());
        auto attached = selection.session.attach_persistence(std::move(queue));
        if (!attached) return lighter::outcome_error(std::move(attached).error());
        selection.notices.push_back("[session not saving: " + opened.error().message() + "]\n");
        return selection;
    }
    auto store = *std::move(opened);

    if (options.kind == StartupKind::NEW) {
        SessionSelection selection;
        selection.session.metadata.workspace = session::SessionWorkspace{.root = workspace.root, .key = workspace.key};
        selection.session.metadata.working_directory = working_directory.generic_string();
        auto writer = store.lease(selection.session.id);
        if (!writer) {
            auto queue = session::PersistenceQueue::create_reopening(*path, selection.session.id, writer.error().message());
            auto attached = selection.session.attach_persistence(std::move(queue));
            if (!attached) return lighter::outcome_error(std::move(attached).error());
            selection.notices.push_back("[session not saving: " + writer.error().message() + "]\n");
        } else {
            auto attached = selection.session.attach_persistence(session::PersistenceQueue::create(*std::move(writer)));
            if (!attached) return lighter::outcome_error(std::move(attached).error());
        }
        return selection;
    }

    session::SessionId selected_id;
    if (options.kind == StartupKind::RESUME) {
        auto resolved = store.resolve_id(options.session);
        if (!resolved) return lighter::outcome_error(std::move(resolved).error());
        selected_id = *resolved;
    } else {
        auto latest = store.latest(workspace.key);
        if (!latest) return lighter::outcome_error(std::move(latest).error());
        selected_id = latest->id;
    }

    auto writer = store.lease(selected_id);
    if (!writer) return lighter::outcome_error(std::move(writer).error());
    auto loaded = writer->load();
    if (!loaded) return lighter::outcome_error(std::move(loaded).error());
    SessionSelection selection{.session = *std::move(loaded)};
    auto queue = session::PersistenceQueue::create(*std::move(writer));
    auto attached = selection.session.attach_persistence(queue);
    if (!attached) return lighter::outcome_error(std::move(attached).error());
    const auto recovered = session::recover_interrupted(selection.session);
    if (recovered.recovered_tasks != 0) {
        selection.notices.push_back("[recovered an interrupted task; tools were not re-executed]\n");
        auto flushed = queue->flush();
        if (!flushed) selection.notices.push_back("[session not saving: " + flushed.error().message() + "]\n");
    }
    if (options.kind == StartupKind::RESUME && selection.session.metadata.workspace &&
        selection.session.metadata.workspace->key != workspace.key) {
        selection.notices.push_back("[workspace warning: session belongs to " + selection.session.metadata.workspace->root +
                                    "; tools will run in " + working_directory.generic_string() + "]\n");
    }
    return selection;
}

lighter::Task<i32> run_app(ToolSet &tools, lighter::InterruptSource &interrupts, model::Catalog &models, const StartupOptions &options) {
    auto project_root = context::discover_project_root(tools.working_directory);
    if (!project_root) {
        std::fprintf(stderr, "error: %s\n", project_root.error().message().c_str());
        co_return 1;
    }
    auto workspace = session::workspace_identity(*project_root);
    if (!workspace) {
        std::fprintf(stderr, "error: %s\n", workspace.error().message().c_str());
        co_return 1;
    }
    auto selected_session = select_session(options, tools.working_directory, *workspace);
    if (!selected_session) {
        std::fprintf(stderr, "error: %s\n", selected_session.error().message().c_str());
        co_return 1;
    }

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
    const auto &metadata = selected_session->session.metadata;
    if (selector.empty() && metadata.model_preference) {
        selector = metadata.model_preference->provider + "/" + metadata.model_preference->model;
        if (metadata.model_preference->reasoning_effort) selector += "@" + *metadata.model_preference->reasoning_effort;
    }
    if (selector.empty()) {
        const auto &first = models.entries().front();
        selector = first.provider + "/" + first.id;
    }
    auto initial = models.select(selector);
    if (!initial) {
        if (!metadata.model_preference || !env_or("LIMINAL_MODEL", "").empty()) {
            std::fprintf(stderr, "error: %s\n", initial.error().message().c_str());
            co_return 1;
        }
        const auto &first = models.entries().front();
        auto fallback = models.select(first.provider + "/" + first.id);
        if (!fallback) {
            std::fprintf(stderr, "error: %s\n", fallback.error().message().c_str());
            co_return 1;
        }
        selected_session->notices.push_back("[stored model " + selector + " is unavailable; using " + first.provider + "/" + first.id +
                                            "]\n");
        initial = *std::move(fallback);
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
    auto project_instructions = context::ProjectInstructionResolver{}.resolve(*project_root, tools.working_directory, instruction_files);
    if (!project_instructions) {
        std::fprintf(stderr, "error: %s\n", project_instructions.error().message().c_str());
        co_return 1;
    }
    for (auto &instruction : *project_instructions) {
        instructions.push_back(std::move(instruction));
    }
    auto selection = *std::move(selected_session);
    Agent agent(*std::move(initial), tools, std::move(instructions), std::move(selection.session));
    co_return co_await tui::run_repl(agent, interrupts, models, std::move(selection.notices));
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
    StartupOptions options;
    if (argc > 1) {
        if (argc == 3 && std::string_view(argv[1]) == "login" && std::string_view(argv[2]) == "codex") {
            return run_codex_login();
        }
        if (argc == 3 && std::string_view(argv[1]) == "resume") {
            options = {.kind = StartupKind::RESUME, .session = argv[2]};
        } else if (argc == 2 && std::string_view(argv[1]) == "continue") {
            options = {.kind = StartupKind::CONTINUE};
        } else {
            std::fprintf(stderr, "usage: %s [login codex | resume <session-id> | continue]\n", argv[0]);
            return 2;
        }
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0); // text deltas must appear immediately

    std::error_code cwd_error;
    auto cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        std::fprintf(stderr, "error: cannot resolve working directory: %s\n", cwd_error.message().c_str());
        return 1;
    }
    // Loop-bound services such as exec sessions must release their libuv
    // handles before the loop itself is torn down.
    lighter::EventLoop loop;
    ToolSet tools(std::move(cwd));
    model::Catalog models(provider::default_providers_file(), codex::default_auth_file());
    auto interrupts = lighter::InterruptSource::create(loop);
    if (!interrupts) {
        std::fprintf(stderr, "error: cannot watch process controls: %s\n", std::string(interrupts.error().message()).c_str());
        return 1;
    }

    auto app = run_app(tools, *interrupts, models, options);
    loop.schedule(app);
    loop.run();
    return static_cast<int>(app.result());
}
