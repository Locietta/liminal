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

session::TaskId append_session_message(session::Session &log, std::string prompt, std::string text, provider::Usage usage = {}) {
    const auto task_id = log.start_task(std::move(prompt));
    log.append(session::OutputItemCompleted{
        .task_id = task_id,
        .provider_call_id = {.value = 1},
        .item = provider::AssistantMessageItem{.id = {.value = "seed"}, .parts = {{.text = std::move(text)}}},
    });
    if (usage.context_tokens != 0 || usage.input_tokens != 0 || usage.output_tokens != 0) {
        log.append(session::ProviderCallCompleted{
            .task_id = task_id,
            .id = {.value = 1},
            .completion = {.usage = usage},
        });
    }
    return task_id;
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

void test_successful_task() {
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
            require(history.size() == 6 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER &&
                        history[2].role == provider::Role::USER && history[3].role == provider::Role::ASSISTANT &&
                        history[4].role == provider::Role::ASSISTANT && history[5].role == provider::Role::USER,
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
    auto task = agent.run_task("inspect", sink);
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_value(), "agent task failed");
    require(events.size() == 7, "agent emitted an unexpected event count");
    require(std::holds_alternative<AssistantTextDelta>(events[0]), "text delta must be the first response event");
    require(std::holds_alternative<AssistantMessageCompleted>(events[1]), "assistant message must finish before its tool");
    require(std::get<AssistantTextDelta>(events[0]).item_id == "message-1" &&
                std::get<AssistantMessageCompleted>(events[1]).item_id == "message-1" &&
                std::get<AssistantMessageCompleted>(events[1]).text == "checking",
            "agent events did not preserve assistant output-item identity");
    require(std::holds_alternative<ToolStarted>(events[2]), "tool start event is missing");
    require(std::holds_alternative<ToolCompleted>(events[3]), "tool completion event is missing");
    require(std::holds_alternative<AssistantTextDelta>(events[4]), "continuation text delta is missing");
    require(std::holds_alternative<AssistantMessageCompleted>(events[5]), "continuation message completion is missing");
    require(std::holds_alternative<TaskCompleted>(events[6]), "task completion event is missing");
    require(agent.session.entries.size() == 8, "semantic session has the wrong number of entries");
    require(std::holds_alternative<session::TaskStarted>(agent.session.entries[0].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[2].payload) &&
                std::holds_alternative<session::ProviderCallCompleted>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::ToolResults>(agent.session.entries[4].payload) &&
                std::holds_alternative<session::TaskFinished>(agent.session.entries[7].payload),
            "session entries do not preserve task, output-item, provider-call, tool-result, and finish semantics");
    const auto &first_call = std::get<session::ProviderCallCompleted>(agent.session.entries[3].payload);
    require(first_call.completion.usage.input_tokens == 10 && first_call.completion.usage.output_tokens == 5 &&
                first_call.completion.model == "fake-model" && first_call.completion.request_id == "request-1",
            "session did not retain normalized response usage and provenance");

    auto compact = agent.compact("keep decisions");
    lighter::EventLoop compact_loop;
    compact_loop.schedule(compact);
    compact_loop.run();
    require(compact.result().has_value(), "agent compaction failed");
    require(agent.session.entries.size() == 9 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries.back().payload),
            "compaction must append a checkpoint without deleting session entries");
    auto compacted_context = agent.context_manifest();
    require(compacted_context && compacted_context->provider_history.size() == 3 &&
                compacted_context->provider_history[0].role == provider::Role::SYSTEM &&
                compacted_context->provider_history[1].role == provider::Role::DEVELOPER &&
                compacted_context->omitted_session_entries == 8 && compacted_context->session_entries.size() == 1,
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
    auto task = agent.run_task("keep working", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a productive tool loop longer than 32 iterations failed");
    require(*tool_executions == k_tool_calls, "the long-running task did not execute every requested tool");
    require(agent.session.entries.size() == 3 * k_tool_calls + 4, "the long-running task did not retain its complete semantic history");
    provider_mock.verify();
}

void test_tool_starts_before_provider_call_finishes() {
    ToolSet tools(std::filesystem::current_path());
    auto executed = std::make_shared<bool>(false);
    auto registered = tools.register_tool({
        .definition = {.name = "test_online", .description = "Test online scheduling"},
        .execute = [executed](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            *executed = true;
            co_return provider::ToolResult{.call_id = call.id, .content = "online"};
        },
    });
    require(registered.has_value(), "failed to register the online scheduling test tool");

    auto completions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls(
            [completions, executed](const provider::History &, const std::vector<provider::ToolDefinition> &,
                                    const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
                if ((*completions)++ != 0) {
                    emit_message(callbacks, "final", "done", true);
                    co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
                }
                emit_message(callbacks, "before", "I will inspect.", true);
                emit_tool_call(callbacks, "online-call", "test_online");
                co_await lighter::sleep(1ms);
                require(*executed, "tool execution waited for the provider call to finish");
                emit_message(callbacks, "after", "The inspection is running.", true);
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            })
        .times(2);
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("work online", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "online scheduling task failed");
    usize tool_completed = events.size();
    usize after_delta = events.size();
    for (usize index = 0; index < events.size(); ++index) {
        if (std::holds_alternative<ToolCompleted>(events[index])) tool_completed = index;
        if (const auto *delta = std::get_if<AssistantTextDelta>(&events[index]); delta && delta->text == "The inspection is running.") {
            after_delta = index;
        }
    }
    require(tool_completed < after_delta, "provider progress emitted before the completed online tool event");
    provider_mock.verify();
}

void test_done_requires_terminal_answer() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            callbacks.on_item_completed(provider::AssistantMessageItem{
                .id = {.value = "commentary"},
                .parts = {{.text = "Still working."}},
                .phase = provider::MessagePhase::COMMENTARY,
            });
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("finish the task", {});
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.has_error() && outcome.error().detail.contains("without a terminal assistant answer"),
            "a provider DONE without a terminal answer completed the task");
    require(std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "missing terminal output did not fail the semantic task");
    provider_mock.verify();
}

void test_final_phase_does_not_bypass_tool_follow_up() {
    ToolSet tools(std::filesystem::current_path());
    auto tool_executions = register_noop_tool(tools);
    auto completions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                provider::OutputItemId item_id{.value = "premature-final"};
                callbacks.on_item_completed(provider::AssistantMessageItem{
                    .id = std::move(item_id),
                    .parts = {{.text = "I found the answer."}},
                    .phase = provider::MessagePhase::FINAL,
                });
                emit_tool_call(callbacks, "call", "test_noop");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            }
            emit_message(callbacks, "actual-final", "Done.");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("verify it", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a final-phase message with tools did not continue to the actual final answer");
    require(*tool_executions == 1 && *completions == 2, "message phase incorrectly overrode task control flow");
    provider_mock.verify();
}

lighter::Task<> cancel_after(lighter::CancellationSource &source, std::chrono::milliseconds delay) {
    co_await lighter::sleep(delay);
    source.cancel();
}

void test_cancelled_task_retains_semantic_progress() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "test_slow", .description = "Test cancellation"},
        .execute = [](const ToolSet &, const provider::ToolCall &call) -> lighter::Task<provider::ToolResult, Error> {
            co_await lighter::sleep(1s);
            co_return provider::ToolResult{.call_id = call.id, .content = "late"};
        },
    });
    require(registered.has_value(), "failed to register the cancellable test tool");

    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_message(callbacks, "progress", "I started the slow work.");
            emit_tool_call(callbacks, "slow-call", "test_slow");
            co_await lighter::sleep(1s);
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::CancellationSource cancellation;
    lighter::EventLoop loop;
    auto task = agent.run_task("start slow work", {}, cancellation.token());
    loop.schedule(task);
    loop.schedule(cancel_after(cancellation, 5ms));
    loop.run();

    auto outcome = task.result();
    require(outcome.is_cancelled(), "task cancellation did not propagate to the caller");
    require(agent.session.entries.size() == 4 && std::holds_alternative<session::TaskStarted>(agent.session.entries[0].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[2].payload) &&
                std::get<session::TaskFinished>(agent.session.entries[3].payload).outcome == session::TaskOutcome::CANCELLED,
            "cancelled task did not retain completed output items and its terminal outcome");
    provider_mock.verify();
}

void test_stream_failure_retains_completed_tool_result() {
    ToolSet tools(std::filesystem::current_path());
    auto tool_executions = register_noop_tool(tools);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [tool_executions](const provider::History &, const std::vector<provider::ToolDefinition> &,
                          const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_tool_call(callbacks, "completed-before-failure", "test_noop");
            co_await lighter::sleep(1ms);
            require(*tool_executions == 1, "online tool did not complete before the simulated stream failure");
            co_await lighter::fail(Error::protocol("simulated stream failure"));
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("do fragile work", {});
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.has_error() && outcome.error().detail == "simulated stream failure",
            "provider stream failure did not reach the caller");
    require(agent.session.entries.size() == 4 && std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::ToolResults>(agent.session.entries[2].payload) &&
                std::get<session::ToolResults>(agent.session.entries[2].payload).results.front().call_id == "completed-before-failure" &&
                std::get<session::TaskFinished>(agent.session.entries[3].payload).outcome == session::TaskOutcome::FAILED,
            "stream failure rolled back a completed tool call or result");
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
    auto task = agent.run_task("schedule tools", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "tool scheduling task failed");
    require(probe->max_running == k_parallel_calls, "parallel-safe tool calls were subject to a numeric concurrency cap");
    require(!probe->exclusive_overlapped, "an exclusive tool overlapped the preceding parallel wave");
    require(!probe->trailing_started_early, "a parallel tool crossed an exclusive ordering barrier");
    const auto result_entry = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolResults>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "tool scheduling task did not retain its result batch");
    const auto &results = std::get<session::ToolResults>(result_entry->payload).results;
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
    append_session_message(agent.session, std::string(100, 'u'), std::string(100, 'a'));

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "automatic compaction task failed");
    require(events.size() == 3 && std::holds_alternative<AssistantMessageCompleted>(events[0]) &&
                std::holds_alternative<SessionNotice>(events[1]) && std::holds_alternative<TaskCompleted>(events[2]),
            "automatic compaction emitted the wrong lifecycle events");
    require(agent.session.entries.size() == 7 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[4].payload) &&
                std::holds_alternative<session::TaskFinished>(agent.session.entries[6].payload),
            "automatic compaction did not commit a semantic checkpoint with the task");
    provider_mock.verify();
}

void test_multiple_automatic_compactions_in_one_task() {
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
    append_session_message(agent.session, std::string(4'000, 'u'), std::string(4'000, 'a'));

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a task requiring multiple automatic compactions failed");
    require(*compactions == 2, "the long-running task did not compact as often as needed");
    require(*tool_executions == 1, "the compacted task did not execute its tool call");
    require(agent.session.entries.size() == 11 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[7].payload),
            "the task did not commit both automatic checkpoints");
    require(std::ranges::count_if(events, [](const Event &event) { return std::holds_alternative<SessionNotice>(event); }) == 1,
            "multiple automatic compactions must emit one committed-task notice");
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
    append_session_message(agent.session, "old", "answer", {.input_tokens = 70, .output_tokens = 18, .context_tokens = 88});

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "proactive automatic compaction task failed");
    require(agent.session.entries.size() == 8 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[4].payload),
            "90 percent context usage did not create an automatic checkpoint");
    provider_mock.verify();
}

void test_failed_task_does_not_report_staged_compaction() {
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
    append_session_message(agent.session, std::string(100, 'u'), std::string(100, 'a'));

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().detail.contains("context window exhausted"),
            "failed completion did not report context exhaustion");
    require(agent.session.entries.size() == 6 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[3].payload) &&
                std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "failed task did not retain its resumable semantic progress");
    require(events.empty(), "failed task emitted an assistant-message boundary without an assistant message");
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
    session_log.start_task("change the project");

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
    test_successful_task();
    test_long_tool_loop();
    test_tool_starts_before_provider_call_finishes();
    test_done_requires_terminal_answer();
    test_final_phase_does_not_bypass_tool_follow_up();
    test_cancelled_task_retains_semantic_progress();
    test_stream_failure_retains_completed_tool_result();
    test_tool_execution_semantics();
    test_automatic_compaction();
    test_multiple_automatic_compactions_in_one_task();
    test_proactive_compaction_uses_reported_context();
    test_failed_task_does_not_report_staged_compaction();
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
