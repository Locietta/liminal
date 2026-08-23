#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xmake/version/liminal.hpp>

#include <lighter/async/async.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/interrupt.h>

#include <liminal/agent/agent.h>
#include <liminal/agent/default_instructions.h>
#include <liminal/application/session_coordinator.h>
#include <liminal/context/context.h>
#include <liminal/context/project_instructions.h>
#include <liminal/model/catalog.h>
#include <liminal/provider/codex_auth.h>
#include <liminal/provider/registry.h>
#include <liminal/session/paths.h>
#include <liminal/session/persistence.h>
#include <liminal/session/catalog.h>
#include <liminal/session/repository.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/hydration.h>
#include <liminal/tui/repl.h>

namespace {

using namespace liminal;

void print_version() {
    std::printf("liminal %.*s", static_cast<int>(xmake::version::version.size()), xmake::version::version.data());
    if (!xmake::version::commit.empty()) {
        const auto short_commit = xmake::version::commit.substr(0, 12);
        std::printf(" (%.*s%s)", static_cast<int>(short_commit.size()), short_commit.data(), xmake::version::dirty ? "-dirty" : "");
    }
    std::putchar('\n');
}

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
    model::Choice model;
    std::vector<tui::Block> transcript;
    std::vector<std::string> notices;
    std::optional<application::SessionCoordinator> coordinator;
};

struct AcquiredStartupSession {
    application::SessionCoordinator coordinator;
    application::AcquiredSession acquired;
};

std::optional<std::string> configured_model_selector() {
    auto configured = env_or("LIMINAL_MODEL", "");
    return configured.empty() ? std::nullopt : std::optional{std::move(configured)};
}

application::SessionPreparationServices preparation_services(model::Catalog &models, ToolSet &tools,
                                                             std::optional<std::string> configured_model) {
    return application::SessionPreparationServices(
        [&tools](const session::Session &value) -> Result<std::vector<tui::Block>> { return tui::project_transcript(value, tools); },
        [&models, configured_model = std::move(configured_model)](const std::optional<session::SessionModelPreference> &stored_model) {
            return application::resolve_session_model(models, configured_model, stored_model);
        });
}

Result<AcquiredStartupSession> acquire_startup_session(const StartupOptions &options, const session::WorkspaceIdentity &workspace,
                                                       model::Catalog &models, ToolSet &tools) {
    auto path = session::state_root_path();
    if (!path) return lighter::outcome_error(std::move(path).error());

    const auto exact_resume = options.kind == StartupKind::RESUME && options.session.size() == 36;
    auto repository = session::SessionRepository::open(*path, exact_resume ? session::RepositoryOpenMode::DEFER_CATALOG_REBUILD :
                                                                             session::RepositoryOpenMode::INITIALIZE_CATALOG);
    if (!repository) return lighter::outcome_error(std::move(repository).error());
    application::SessionCoordinator coordinator(*repository, preparation_services(models, tools, configured_model_selector()));

    auto acquired = [&]() -> Result<application::AcquiredSession> {
        if (options.kind != StartupKind::RESUME) return coordinator.acquire_latest(workspace.key);
        const auto exact = options.session.size() == 36;
        if (!exact) {
            auto catalog = repository->catalog();
            if (!catalog) return lighter::outcome_error(std::move(catalog).error());
            auto resolved = catalog->resolve_prefix(options.session);
            if (!resolved) return lighter::outcome_error(std::move(resolved).error());
            return coordinator.acquire_catalog_hint(*resolved);
        }
        auto resolved = repository->resolve_exact(options.session);
        if (!resolved) return lighter::outcome_error(std::move(resolved).error());
        return coordinator.acquire(*resolved);
    }();
    if (!acquired) return lighter::outcome_error(std::move(acquired).error());
    for (const auto &warning : repository->warnings()) acquired->notices.push_back("[session catalog warning: " + warning + "]\n");
    return AcquiredStartupSession{.coordinator = std::move(coordinator), .acquired = *std::move(acquired)};
}

Result<SessionSelection> finish_startup_session(AcquiredStartupSession startup, const StartupOptions &options,
                                                const std::filesystem::path &working_directory,
                                                const session::WorkspaceIdentity &workspace) {
    auto prepared = startup.coordinator.resolve_model(std::move(startup.acquired));
    if (!prepared) return lighter::outcome_error(std::move(prepared).error());
    SessionSelection selection{
        .session = std::move(prepared->session),
        .model = std::move(prepared->model),
        .transcript = std::move(prepared->transcript),
        .notices = std::move(prepared->notices),
        .coordinator = std::move(startup.coordinator),
    };
    if (options.kind == StartupKind::RESUME && selection.session.metadata.workspace &&
        selection.session.metadata.workspace->key != workspace.key) {
        selection.notices.push_back("[workspace warning: session belongs to " + selection.session.metadata.workspace->root +
                                    "; tools will run in " + working_directory.generic_string() + "]\n");
    }
    return selection;
}

Result<SessionSelection> create_session(const std::filesystem::path &working_directory, const session::WorkspaceIdentity &workspace,
                                        model::Catalog &models, ToolSet &tools) {
    auto resolved_model = application::resolve_session_model(models, configured_model_selector(), std::nullopt);
    if (!resolved_model) return lighter::outcome_error(std::move(resolved_model).error());
    SessionSelection selection{.model = std::move(resolved_model->model)};
    selection.session.metadata.workspace = session::SessionWorkspace{.root = workspace.root, .key = workspace.key};
    selection.session.metadata.working_directory = working_directory.generic_string();

    auto path = session::state_root_path();
    if (!path) {
        auto attached =
            selection.session.attach_persistence(session::PersistenceQueue::create_resolving(selection.session.id, path.error().message()));
        if (!attached) return lighter::outcome_error(std::move(attached).error());
        selection.notices.push_back("[session not saving: " + path.error().message() + "]\n");
        return selection;
    }

    auto repository = session::SessionRepository::open(*path);
    if (!repository) {
        auto queue = session::PersistenceQueue::create_reopening(*path, selection.session.id, repository.error().message());
        auto attached = selection.session.attach_persistence(std::move(queue));
        if (!attached) return lighter::outcome_error(std::move(attached).error());
        selection.notices.push_back("[session not saving: " + repository.error().message() + "]\n");
        return selection;
    }
    for (const auto &warning : repository->warnings()) selection.notices.push_back("[session catalog warning: " + warning + "]\n");
    selection.coordinator.emplace(*repository, preparation_services(models, tools, configured_model_selector()));
    auto writer = repository->create(selection.session.id);
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

    std::optional<AcquiredStartupSession> acquired_session;
    if (options.kind != StartupKind::NEW) {
        auto acquired = acquire_startup_session(options, *workspace, models, tools);
        if (!acquired) {
            std::fprintf(stderr, "error: %s\n", acquired.error().message().c_str());
            co_return 1;
        }
        acquired_session = *std::move(acquired);
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

    auto selected_session = acquired_session ?
                                finish_startup_session(*std::move(acquired_session), options, tools.working_directory, *workspace) :
                                create_session(tools.working_directory, *workspace, models, tools);
    if (!selected_session) {
        std::fprintf(stderr, "error: %s\n", selected_session.error().message().c_str());
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
    auto project_instructions = context::ProjectInstructionResolver{}.resolve(*project_root, tools.working_directory, instruction_files);
    if (!project_instructions) {
        std::fprintf(stderr, "error: %s\n", project_instructions.error().message().c_str());
        co_return 1;
    }
    for (auto &instruction : *project_instructions) {
        instructions.push_back(std::move(instruction));
    }
    auto selection = *std::move(selected_session);
    auto coordinator = std::move(selection.coordinator);
    Agent agent(std::move(selection.model), tools, std::move(instructions), std::move(selection.session));
    co_return co_await tui::run_repl(agent, interrupts, models, coordinator ? &*coordinator : nullptr,
                                     session::SessionWorkspace{.root = workspace->root, .key = workspace->key},
                                     std::move(selection.transcript), std::move(selection.notices));
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
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        print_version();
        return 0;
    }

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
            std::fprintf(stderr, "usage: %s [--version | login codex | resume <session-id> | continue]\n", argv[0]);
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
