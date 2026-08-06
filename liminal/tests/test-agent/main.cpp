#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>
#include <proxy/proxy.h>

#include <lighter/async/io/loop.h>
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

struct FakeProvider {
    lighter::Task<provider::TurnResponse, Error> complete(const provider::History &history, const std::vector<provider::ToolDefinition> &,
                                                          const provider::StreamCallbacks &callbacks) {
        require(history.size() >= 2 && history[0].role == provider::Role::DEVELOPER,
                "each request must begin with the agent's developer instruction");
        require(history[1].role == provider::Role::USER, "the task must remain a user message");
        if (calls++ == 0) {
            callbacks.on_text_delta("checking");
            glz::generic input;
            auto parse_error = glz::read_json(input, R"({"path":"README.md"})");
            require(!parse_error, "failed to create read_file input");

            provider::TurnResponse response{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            response.parts.push_back(provider::TextPart{.text = "checking"});
            response.parts.push_back(provider::ToolCall{
                .id = "call-1",
                .name = "read_file",
                .input = std::move(input),
            });
            co_return response;
        }

        require(history.size() == 4 && history[2].role == provider::Role::ASSISTANT && history[3].role == provider::Role::USER,
                "generated output and tool results used the wrong message roles");

        callbacks.on_text_delta("done");
        provider::TurnResponse response{.stop = provider::StopKind::DONE};
        response.parts.push_back(provider::TextPart{.text = "done"});
        co_return response;
    }

    lighter::Task<void, Error> compact(provider::History &history, std::string_view) {
        const auto instruction_count = provider::instruction_prefix_size(history);
        history.resize(instruction_count);
        provider::append_user(history, "SUMMARY");
        co_return;
    }

    usize calls = 0;
};

struct OverBudgetProvider {
    lighter::Task<provider::TurnResponse, Error> complete(const provider::History &, const std::vector<provider::ToolDefinition> &,
                                                          const provider::StreamCallbacks &) {
        provider::TurnResponse response{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        for (usize index = 0; index < 2; ++index) {
            glz::generic input;
            auto parse_error = glz::read_json(input, R"({"path":"README.md"})");
            require(!parse_error, "failed to create budget test input");
            response.parts.push_back(provider::ToolCall{
                .id = "call-" + std::to_string(index),
                .name = "read_file",
                .input = std::move(input),
            });
        }
        co_return response;
    }

    lighter::Task<void, Error> compact(provider::History &, std::string_view) { co_return; }
};

void test_successful_turn() {
    ToolSet tools(std::filesystem::current_path().string());
    model::Choice choice{
        .handle = pro::make_proxy<provider::ProviderFacade, FakeProvider>(),
        .entry = {.provider = "fake", .id = "test"},
    };
    Agent agent(std::move(choice), tools);
    require(agent.conversation.empty(), "instructions must not be stored as conversation");
    auto initial_context = agent.context_manifest();
    require(initial_context && initial_context->instructions.size() == 1 &&
                initial_context->instructions[0].origin == "builtin:default-agent" &&
                initial_context->provider_history[0].role == provider::Role::DEVELOPER,
            "an agent must resolve its profile prompt as a sourced developer instruction");

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
    require(agent.conversation.size() == 4, "transactional conversation has the wrong number of items");
    require(agent.conversation[0].role == provider::Role::USER && agent.conversation[1].role == provider::Role::ASSISTANT &&
                agent.conversation[2].role == provider::Role::USER && agent.conversation[3].role == provider::Role::ASSISTANT,
            "conversation roles do not preserve user, assistant, tool-result, assistant order");

    auto compact = agent.compact("keep decisions");
    lighter::EventLoop compact_loop;
    compact_loop.schedule(compact);
    compact_loop.run();
    require(compact.result().has_value(), "agent compaction failed");
    require(agent.conversation.size() == 1 && agent.conversation[0].role == provider::Role::USER,
            "compaction must commit only mutable conversation state");
    auto compacted_context = agent.context_manifest();
    require(compacted_context && compacted_context->provider_history.size() == 2 &&
                compacted_context->provider_history[0].role == provider::Role::DEVELOPER,
            "compaction did not reconstruct the instruction prefix");
}

void test_tool_call_budget() {
    ToolPolicy policy{.max_calls_per_turn = 1};
    ToolSet tools(std::filesystem::current_path(), policy);
    model::Choice choice{
        .handle = pro::make_proxy<provider::ProviderFacade, OverBudgetProvider>(),
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
    require(agent.conversation.empty(), "an over-budget turn must not commit conversation state");
}

void test_context_manifest() {
    std::vector<context::InstructionSource> instructions{
        {.authority = context::InstructionAuthority::PROJECT, .origin = "project:AGENTS.md", .content = "project policy"},
        {.authority = context::InstructionAuthority::APPLICATION, .origin = "profile:coding", .content = "coding policy"},
        {.authority = context::InstructionAuthority::RUNTIME, .origin = "runtime:liminal", .content = "platform policy"},
        {.authority = context::InstructionAuthority::PROJECT, .origin = "project:duplicate", .content = "project policy"},
    };
    provider::History conversation;
    provider::append_user(conversation, "change the project");

    auto manifest = context::ContextBuilder{}.build(instructions, conversation);
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

    provider::append_developer(conversation, "misplaced policy");
    auto invalid = context::ContextBuilder{}.build(instructions, conversation);
    require(!invalid && invalid.error().detail.contains("must use sourced context"),
            "context builder accepted an instruction in mutable conversation");

    auto altered_history = manifest->provider_history;
    std::get<provider::TextPart>(altered_history.front().parts.front()).text = "altered platform policy";
    auto altered = context::ContextBuilder{}.take_conversation(std::move(altered_history), *manifest);
    require(!altered && altered.error().detail.contains("changed the resolved instruction prefix"),
            "context builder accepted altered instructions from a provider operation");
}

i32 run_all() {
    test_successful_turn();
    test_tool_call_budget();
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
