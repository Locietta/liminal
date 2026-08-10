#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/mock/mock.h>
#include <lighter/types.hpp>

#include <liminal/agent/agent.h>
#include <liminal/event.h>
#include <liminal/provider/provider.h>

namespace {

using namespace lighter::types;
using namespace liminal;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

glz::generic read_file_input() {
    glz::generic input;
    auto parse_error = glz::read_json(input, R"({"path":"README.md"})");
    require(!parse_error, "failed to create read_file input");
    return input;
}

std::vector<context::InstructionSource> compact_test_instructions() {
    return {{
        .authority = context::InstructionAuthority::APPLICATION,
        .origin = "test:compact",
        .content = "test policy",
    }};
}

void test_successful_turn() {
    ToolSet tools(std::filesystem::current_path().string());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .then_calls([](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::TurnResponse, Error> {
            require(history.size() == 3 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER &&
                        history[2].role == provider::Role::USER,
                    "initial request used the wrong instruction or task roles");
            callbacks.on_text_delta("checking");
            provider::TurnResponse response{
                .stop = provider::StopKind::NEEDS_TOOL_RESULTS,
                .usage = {.input_tokens = 10, .output_tokens = 5},
                .model = "fake-model",
                .request_id = "request-1",
            };
            response.parts.push_back(provider::TextPart{.text = "checking"});
            response.parts.push_back(provider::ToolCall{
                .id = "call-1",
                .name = "read_file",
                .input = read_file_input(),
            });
            co_return response;
        })
        .then_calls([](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::TurnResponse, Error> {
            require(history.size() == 5 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER &&
                        history[2].role == provider::Role::USER && history[3].role == provider::Role::ASSISTANT &&
                        history[4].role == provider::Role::USER,
                    "tool continuation used the wrong message roles");
            callbacks.on_text_delta("done");
            provider::TurnResponse response{
                .stop = provider::StopKind::DONE,
                .usage = {.input_tokens = 20, .output_tokens = 3},
                .model = "fake-model",
                .request_id = "request-2",
            };
            response.parts.push_back(provider::TextPart{.text = "done"});
            co_return response;
        });
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, std::string_view instructions) -> lighter::Task<void, Error> {
            require(instructions == "keep decisions", "compaction received the wrong instructions");
            const auto instruction_count = provider::instruction_prefix_size(history);
            history.resize(instruction_count);
            provider::append_user(history, "SUMMARY");
            co_return;
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test"},
    };
    Agent agent(std::move(choice), tools);
    require(agent.session.entries.empty(), "instructions must not be stored as session entries");
    auto initial_context = agent.context_manifest();
    require(initial_context && initial_context->instructions.size() == 2 &&
                initial_context->instructions[0].origin == "builtin:runtime-tools" &&
                initial_context->instructions[0].content.contains("`rg --files`") &&
                initial_context->instructions[0].content.contains("uutils") &&
                initial_context->instructions[0].content.contains("`apply_patch`") &&
                initial_context->instructions[1].origin == "builtin:default-agent" &&
                initial_context->provider_history[0].role == provider::Role::SYSTEM &&
                initial_context->provider_history[1].role == provider::Role::DEVELOPER,
            "an agent must resolve its runtime and application instructions");

    std::vector<Event> events;
    EventSink sink = [&events](const Event &event) { events.push_back(event); };

    lighter::EventLoop loop;
    auto task = agent.run_turn("inspect", sink);
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_value(), "agent turn failed");
    require(events.size() == 7, "agent emitted an unexpected event count");
    require(std::holds_alternative<AssistantTextDelta>(events[0]), "text delta must be the first response event");
    require(std::holds_alternative<AssistantSegmentCompleted>(events[1]), "assistant segment must finish before its tool");
    require(std::holds_alternative<ToolStarted>(events[2]), "tool start event is missing");
    require(std::holds_alternative<ToolCompleted>(events[3]), "tool completion event is missing");
    require(std::holds_alternative<AssistantTextDelta>(events[4]), "continuation text delta is missing");
    require(std::holds_alternative<AssistantSegmentCompleted>(events[5]), "continuation segment completion is missing");
    require(std::holds_alternative<TurnCompleted>(events[6]), "turn completion event is missing");
    require(agent.session.entries.size() == 4, "transactional session has the wrong number of entries");
    require(std::holds_alternative<session::UserMessage>(agent.session.entries[0].payload) &&
                std::holds_alternative<session::AgentOutput>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::ToolResults>(agent.session.entries[2].payload) &&
                std::holds_alternative<session::AgentOutput>(agent.session.entries[3].payload),
            "session entries do not preserve user, agent-output, tool-result, agent-output semantics");
    const auto &first_output = std::get<session::AgentOutput>(agent.session.entries[1].payload);
    require(first_output.usage.input_tokens == 10 && first_output.usage.output_tokens == 5 && first_output.model == "fake-model" &&
                first_output.request_id == "request-1",
            "session did not retain normalized response usage and provenance");

    auto compact = agent.compact("keep decisions");
    lighter::EventLoop compact_loop;
    compact_loop.schedule(compact);
    compact_loop.run();
    require(compact.result().has_value(), "agent compaction failed");
    require(agent.session.entries.size() == 5 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries.back().payload),
            "compaction must append a checkpoint without deleting session entries");
    auto compacted_context = agent.context_manifest();
    require(compacted_context && compacted_context->provider_history.size() == 3 &&
                compacted_context->provider_history[0].role == provider::Role::SYSTEM &&
                compacted_context->provider_history[1].role == provider::Role::DEVELOPER &&
                compacted_context->omitted_session_entries == 4 && compacted_context->session_entries.size() == 1,
            "compaction did not reconstruct the instruction prefix");
    provider_mock.verify();
}

void test_tool_call_budget() {
    ToolPolicy policy{.max_calls_per_turn = 1};
    ToolSet tools(std::filesystem::current_path(), policy);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &) -> lighter::Task<provider::TurnResponse, Error> {
            provider::TurnResponse response{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            for (usize index = 0; index < 2; ++index) {
                response.parts.push_back(provider::ToolCall{
                    .id = "call-" + std::to_string(index),
                    .name = "read_file",
                    .input = read_file_input(),
                });
            }
            co_return response;
        });
    provider_mock.expect<provider::CompactDispatch>().never();
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test"},
    };
    Agent agent(std::move(choice), tools);

    lighter::EventLoop loop;
    auto task = agent.run_turn("too many tools", {});
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().kind == ErrorKind::TOOL && outcome.error().detail.contains("budget exceeded"),
            "an over-budget provider response must fail before executing tools");
    require(agent.session.entries.empty(), "an over-budget turn must not commit session entries");
    provider_mock.verify();
}

void test_automatic_compaction() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, std::string_view instructions) -> lighter::Task<void, Error> {
            require(instructions.contains("active task"), "automatic compaction received the wrong instructions");
            require(history.size() == 4 && history.back().role == provider::Role::USER,
                    "automatic compaction did not receive the full unbounded branch");
            auto latest = std::move(history.back());
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            history.push_back(std::move(latest));
            co_return;
        });
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &history, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &) -> lighter::Task<provider::TurnResponse, Error> {
            require(history.size() == 3 && history[0].role == provider::Role::DEVELOPER && history[1].role == provider::Role::USER &&
                        history[2].role == provider::Role::USER && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history[2].parts[0]).text == "latest",
                    "completion did not use the compacted checkpoint and current prompt");
            co_return provider::TurnResponse{.parts = {provider::TextPart{.text = "done"}}, .stop = provider::StopKind::DONE};
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 80, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    agent.session.append(session::UserMessage{.text = std::string(100, 'u')});
    agent.session.append(session::AgentOutput{.parts = {provider::TextPart{.text = std::string(100, 'a')}}});

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_turn("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "automatic compaction turn failed");
    require(events.size() == 3 && std::holds_alternative<AssistantSegmentCompleted>(events[0]) &&
                std::holds_alternative<SessionNotice>(events[1]) && std::holds_alternative<TurnCompleted>(events[2]),
            "automatic compaction emitted the wrong lifecycle events");
    require(agent.session.entries.size() == 5 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::AgentOutput>(agent.session.entries[4].payload),
            "automatic compaction did not commit a semantic checkpoint with the turn");
    provider_mock.verify();
}

void test_proactive_compaction_uses_reported_context() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls([](provider::History &history, std::string_view) -> lighter::Task<void, Error> {
        require(history.size() == 4 && history.back().role == provider::Role::USER,
                "proactive compaction did not receive the full current context");
        auto latest = std::move(history.back());
        history.resize(provider::instruction_prefix_size(history));
        provider::append_user(history, "SUMMARY");
        history.push_back(std::move(latest));
        co_return;
    });
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &history, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &) -> lighter::Task<provider::TurnResponse, Error> {
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY",
                    "proactive compaction was not used for completion");
            co_return provider::TurnResponse{.parts = {provider::TextPart{.text = "done"}}, .stop = provider::StopKind::DONE};
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 100, .max_output_tokens = 1},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    agent.session.append(session::UserMessage{.text = "old"});
    agent.session.append(session::AgentOutput{
        .parts = {provider::TextPart{.text = "answer"}},
        .usage = {.input_tokens = 70, .output_tokens = 18, .context_tokens = 88},
    });

    lighter::EventLoop loop;
    auto task = agent.run_turn("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "proactive automatic compaction turn failed");
    require(agent.session.entries.size() == 5 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload),
            "90 percent context usage did not create an automatic checkpoint");
    provider_mock.verify();
}

void test_failed_turn_does_not_report_staged_compaction() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls([](provider::History &history, std::string_view) -> lighter::Task<void, Error> {
        auto latest = std::move(history.back());
        history.resize(provider::instruction_prefix_size(history));
        provider::append_user(history, "SUMMARY");
        history.push_back(std::move(latest));
        co_return;
    });
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &) -> lighter::Task<provider::TurnResponse, Error> {
            co_return provider::TurnResponse{.stop = provider::StopKind::CONTEXT_EXHAUSTED};
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 80, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    agent.session.append(session::UserMessage{.text = std::string(100, 'u')});
    agent.session.append(session::AgentOutput{.parts = {provider::TextPart{.text = std::string(100, 'a')}}});

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_turn("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().detail.contains("context window exhausted"),
            "failed completion did not report context exhaustion");
    require(agent.session.entries.size() == 2, "failed turn committed its staged compaction checkpoint");
    require(events.size() == 1 && std::holds_alternative<AssistantSegmentCompleted>(events[0]),
            "failed turn reported an uncommitted automatic compaction");
    provider_mock.verify();
}

void test_context_manifest() {
    std::vector<context::InstructionSource> instructions{
        {.authority = context::InstructionAuthority::PROJECT, .origin = "project:AGENTS.md", .content = "project policy"},
        {.authority = context::InstructionAuthority::APPLICATION, .origin = "profile:coding", .content = "coding policy"},
        {.authority = context::InstructionAuthority::RUNTIME, .origin = "runtime:liminal", .content = "platform policy"},
        {.authority = context::InstructionAuthority::PROJECT, .origin = "project:duplicate", .content = "project policy"},
    };
    session::Session session_log({.value = 42});
    session_log.append(session::UserMessage{.text = "change the project"});

    auto manifest = context::ContextBuilder{}.build(instructions, session_log);
    require(manifest.has_value(), "failed to build a valid context manifest");
    require(manifest->instructions.size() == 3 && manifest->omitted_duplicates.size() == 1, "context instructions were not deduplicated");
    require(manifest->instructions[0].origin == "runtime:liminal" && manifest->instructions[1].origin == "profile:coding" &&
                manifest->instructions[2].origin == "project:AGENTS.md",
            "instruction authority did not produce stable context order");
    require(manifest->provider_history.size() == 4 && manifest->provider_history[0].role == provider::Role::SYSTEM &&
                manifest->provider_history[1].role == provider::Role::DEVELOPER &&
                manifest->provider_history[2].role == provider::Role::DEVELOPER &&
                manifest->provider_history[3].role == provider::Role::USER,
            "context manifest lowered semantic instruction authority incorrectly");
    require(manifest->session_id.value == 42 && manifest->session_entries.size() == 1 && manifest->session_entries[0].value == 1,
            "context manifest did not report its selected session entries");

    auto altered_history = manifest->provider_history;
    std::get<provider::TextPart>(altered_history.front().parts.front()).text = "altered platform policy";
    auto altered = context::ContextBuilder{}.take_checkpoint(std::move(altered_history), *manifest);
    require(!altered && altered.error().detail.contains("changed the resolved instruction prefix"),
            "context builder accepted altered instructions from a provider operation");
}

i32 run_all() {
    test_successful_turn();
    test_tool_call_budget();
    test_automatic_compaction();
    test_proactive_compaction_uses_reported_context();
    test_failed_turn_does_not_report_staged_compaction();
    test_context_manifest();
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
