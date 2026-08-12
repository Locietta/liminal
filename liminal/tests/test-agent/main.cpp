#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
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
using namespace std::chrono_literals;

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

void emit_message(const provider::StreamCallbacks &callbacks, std::string id, std::string text, bool stream = false) {
    provider::OutputItemId item_id{.value = std::move(id)};
    if (stream && callbacks.on_assistant_text_delta) callbacks.on_assistant_text_delta(item_id, text);
    if (callbacks.on_item_completed) {
        callbacks.on_item_completed(provider::AssistantMessageItem{.id = std::move(item_id), .parts = {{.text = std::move(text)}}});
    }
}

void emit_tool_call(const provider::StreamCallbacks &callbacks, std::string id, std::string name, glz::generic input = {}) {
    if (callbacks.on_item_completed) {
        callbacks.on_item_completed(provider::ToolCallItem{
            .id = {.value = "item-" + id},
            .call = {.id = std::move(id), .name = std::move(name), .input = std::move(input)},
        });
    }
}

std::vector<context::InstructionSource> compact_test_instructions() {
    return {{
        .authority = context::InstructionAuthority::APPLICATION,
        .origin = "test:compact",
        .content = "test policy",
    }};
}

std::shared_ptr<usize> register_noop_tool(ToolSet &tools) {
    auto executions = std::make_shared<usize>(0);
    auto registered = tools.register_tool({
        .definition = {.name = "test_noop", .description = "Test no-op"},
        .execute = [executions](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            ++*executions;
            co_return provider::ToolResult{.call_id = call.id, .content = "ok"};
        },
    });
    require(registered.has_value(), "failed to register the test no-op tool");
    return executions;
}

void test_successful_turn() {
    ToolSet tools(std::filesystem::current_path().string());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .then_calls([](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            require(history.size() == 3 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER &&
                        history[2].role == provider::Role::USER,
                    "initial request used the wrong instruction or task roles");
            emit_message(callbacks, "message-1", "checking", true);
            emit_tool_call(callbacks, "call-1", "read_file", read_file_input());
            co_return provider::ProviderCallCompletion{
                .stop = provider::StopKind::NEEDS_TOOL_RESULTS,
                .usage = {.input_tokens = 10, .output_tokens = 5},
                .model = "fake-model",
                .request_id = "request-1",
            };
        })
        .then_calls([](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            require(history.size() == 5 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER &&
                        history[2].role == provider::Role::USER && history[3].role == provider::Role::ASSISTANT &&
                        history[4].role == provider::Role::USER,
                    "tool continuation used the wrong message roles");
            emit_message(callbacks, "message-2", "done", true);
            co_return provider::ProviderCallCompletion{
                .stop = provider::StopKind::DONE,
                .usage = {.input_tokens = 20, .output_tokens = 3},
                .model = "fake-model",
                .request_id = "request-2",
            };
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

void test_long_tool_loop() {
    constexpr usize k_tool_calls = 40;
    ToolSet tools(std::filesystem::current_path());
    auto tool_executions = register_noop_tool(tools);
    auto completions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            const auto completion = (*completions)++;
            if (completion == k_tool_calls) {
                emit_message(callbacks, "done", "done");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            }
            emit_tool_call(callbacks, "call-" + std::to_string(completion), "test_noop");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        })
        .times(k_tool_calls + 1);
    provider_mock.expect<provider::CompactDispatch>().never();
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test"},
    };
    Agent agent(std::move(choice), tools);

    lighter::EventLoop loop;
    auto task = agent.run_turn("keep working", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a productive tool loop longer than 32 iterations failed");
    require(*tool_executions == k_tool_calls, "the long-running turn did not execute every requested tool");
    require(agent.session.entries.size() == 2 * k_tool_calls + 2, "the long-running turn did not commit its complete history");
    provider_mock.verify();
}

struct ToolSchedulingProbe {
    usize running = 0;
    usize max_running = 0;
    bool exclusive_overlapped = false;
    bool exclusive_finished = false;
    bool trailing_started_early = false;
};

void test_tool_execution_semantics() {
    constexpr usize k_parallel_calls = 40;
    ToolSet tools(std::filesystem::current_path());
    auto probe = std::make_shared<ToolSchedulingProbe>();
    auto parallel = tools.register_tool({
        .definition = {.name = "test_parallel", .description = "Test parallel scheduling"},
        .execution_mode = ToolExecutionMode::PARALLEL,
        .execute = [probe](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            if (call.id == "trailing" && !probe->exclusive_finished) probe->trailing_started_early = true;
            ++probe->running;
            probe->max_running = std::max(probe->max_running, probe->running);
            co_await lighter::sleep(5ms);
            --probe->running;
            co_return provider::ToolResult{.call_id = call.id, .content = "parallel"};
        },
    });
    require(parallel.has_value(), "failed to register the parallel scheduling test tool");
    auto exclusive = tools.register_tool({
        .definition = {.name = "test_exclusive", .description = "Test exclusive scheduling"},
        .execute = [probe](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            probe->exclusive_overlapped = probe->running != 0;
            co_await lighter::sleep(1ms);
            probe->exclusive_finished = true;
            co_return provider::ToolResult{.call_id = call.id, .content = "exclusive"};
        },
    });
    require(exclusive.has_value(), "failed to register the exclusive scheduling test tool");

    auto completions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ != 0) {
                emit_message(callbacks, "done", "done");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            }
            for (usize index = 0; index < k_parallel_calls; ++index) {
                emit_tool_call(callbacks, "parallel-" + std::to_string(index), "test_parallel");
            }
            emit_tool_call(callbacks, "exclusive", "test_exclusive");
            emit_tool_call(callbacks, "trailing", "test_parallel");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        })
        .times(2);
    provider_mock.expect<provider::CompactDispatch>().never();
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test"},
    };
    Agent agent(std::move(choice), tools);

    lighter::EventLoop loop;
    auto task = agent.run_turn("schedule tools", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "tool scheduling turn failed");
    require(probe->max_running == k_parallel_calls, "parallel-safe tool calls were subject to a numeric concurrency cap");
    require(!probe->exclusive_overlapped, "an exclusive tool overlapped the preceding parallel wave");
    require(!probe->trailing_started_early, "a parallel tool crossed an exclusive ordering barrier");
    const auto &results = std::get<session::ToolResults>(agent.session.entries[2].payload).results;
    require(results.size() == k_parallel_calls + 2 && results.front().call_id == "parallel-0" &&
                results[k_parallel_calls - 1].call_id == "parallel-39" && results[k_parallel_calls].call_id == "exclusive" &&
                results.back().call_id == "trailing",
            "concurrent tool execution did not preserve model call order");
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
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            require(history.size() == 3 && history[0].role == provider::Role::DEVELOPER && history[1].role == provider::Role::USER &&
                        history[2].role == provider::Role::USER && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history[2].parts[0]).text == "latest",
                    "completion did not use the compacted checkpoint and current prompt");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
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

void test_multiple_automatic_compactions_in_one_turn() {
    ToolSet tools(std::filesystem::current_path());
    auto tool_executions = register_noop_tool(tools);
    auto compactions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>()
        .calls([compactions](provider::History &history, std::string_view) -> lighter::Task<void, Error> {
            const auto instruction_count = provider::instruction_prefix_size(history);
            history.resize(instruction_count);
            provider::append_user(history, "SUMMARY " + std::to_string(++*compactions));
            co_return;
        })
        .times(2);
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            const auto completion = (*completions)++;
            require(history.size() == 2 && history[0].role == provider::Role::DEVELOPER && history[1].role == provider::Role::USER,
                    "completion did not use the latest automatic checkpoint");
            require(std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY " + std::to_string(completion + 1),
                    "completion used the wrong automatic checkpoint");
            if (completion == 0) {
                emit_tool_call(callbacks, "call", "test_noop");
                co_return provider::ProviderCallCompletion{
                    .stop = provider::StopKind::NEEDS_TOOL_RESULTS,
                    .usage = {.input_tokens = 1'800, .output_tokens = 10, .context_tokens = 1'810},
                };
            }
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 2'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    agent.session.append(session::UserMessage{.text = std::string(4'000, 'u')});
    agent.session.append(session::AgentOutput{.parts = {provider::TextPart{.text = std::string(4'000, 'a')}}});

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_turn("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a turn requiring multiple automatic compactions failed");
    require(*compactions == 2, "the long-running turn did not compact as often as needed");
    require(*tool_executions == 1, "the compacted turn did not execute its tool call");
    require(agent.session.entries.size() == 8 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload),
            "the turn did not commit both automatic checkpoints");
    require(std::ranges::count_if(events, [](const Event &event) { return std::holds_alternative<SessionNotice>(event); }) == 1,
            "multiple automatic compactions must emit one committed-turn notice");
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
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY",
                    "proactive compaction was not used for completion");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
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
           const provider::StreamCallbacks &) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::CONTEXT_EXHAUSTED};
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
    test_long_tool_loop();
    test_tool_execution_semantics();
    test_automatic_compaction();
    test_multiple_automatic_compactions_in_one_turn();
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
