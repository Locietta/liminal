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
#include <lighter/encoding/utf8.h>
#include <lighter/mock/mock.h>
#include <lighter/types.hpp>

#include <liminal/agent/agent.h>
#include <liminal/agent/tool_planner.h>
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
    log.append(session::ProviderCallCompleted{
        .task_id = task_id,
        .id = {.value = 1},
        .completion = {.usage = usage},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    log.append(session::ProviderRoundSettled{
        .task_id = task_id,
        .provider_call_id = {.value = 1},
        .replay = session::ProviderRoundReplay::REPLAY,
    });
    log.append(session::TaskFinished{.id = task_id});
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
        .execute = [executions](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            ++*executions;
            co_return ToolOutcome{.call_id = call.id, .payload = "ok"};
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
    require(events.size() == 9, "agent emitted an unexpected event count");
    require(std::holds_alternative<AssistantTextDelta>(events[0]), "text delta must be the first response event");
    require(std::holds_alternative<AssistantMessageCompleted>(events[1]), "assistant message must finish before its tool");
    require(std::get<AssistantTextDelta>(events[0]).item_id == "message-1" &&
                std::get<AssistantMessageCompleted>(events[1]).item_id == "message-1" &&
                std::get<AssistantMessageCompleted>(events[1]).text == "checking",
            "agent events did not preserve assistant output-item identity");
    require(std::holds_alternative<ToolStarted>(events[2]), "tool start event is missing");
    require(std::holds_alternative<ToolCompleted>(events[3]), "tool completion event is missing");
    require(std::holds_alternative<ProviderActivityCompleted>(events[4]), "first provider activity completion is missing");
    require(std::holds_alternative<AssistantTextDelta>(events[5]), "continuation text delta is missing");
    require(std::holds_alternative<AssistantMessageCompleted>(events[6]), "continuation message completion is missing");
    require(std::holds_alternative<ProviderActivityCompleted>(events[7]), "continuation provider activity completion is missing");
    require(std::holds_alternative<TaskCompleted>(events[8]), "task completion event is missing");
    const auto first_scope = std::get<AssistantTextDelta>(events[0]).activity_scope;
    const auto second_scope = std::get<AssistantTextDelta>(events[5]).activity_scope;
    require(first_scope.task_generation != 0 && first_scope.task_generation == second_scope.task_generation &&
                first_scope.provider_call_generation != second_scope.provider_call_generation &&
                std::get<ToolStarted>(events[2]).activity_scope == first_scope &&
                std::get<ToolCompleted>(events[3]).activity_scope == first_scope &&
                std::get<TaskCompleted>(events[8]).task_generation == first_scope.task_generation,
            "agent lifecycle events must preserve task identity while advancing provider-call activity generations");
    require(agent.session.entries.size() == 10, "semantic session has the wrong number of entries");
    require(std::holds_alternative<session::TaskStarted>(agent.session.entries[0].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[2].payload) &&
                std::holds_alternative<session::ProviderCallCompleted>(agent.session.entries[3].payload) &&
                std::holds_alternative<session::ToolOutcomes>(agent.session.entries[4].payload) &&
                std::holds_alternative<session::ProviderRoundSettled>(agent.session.entries[5].payload) &&
                std::holds_alternative<session::TaskFinished>(agent.session.entries[9].payload),
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
    require(agent.session.entries.size() == 11 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries.back().payload),
            "compaction must append a checkpoint without deleting session entries");
    auto compacted_context = agent.context_manifest();
    require(compacted_context && compacted_context->provider_history.size() == 3 &&
                compacted_context->provider_history[0].role == provider::Role::SYSTEM &&
                compacted_context->provider_history[1].role == provider::Role::DEVELOPER &&
                compacted_context->omitted_session_entries == 10 && compacted_context->session_entries.size() == 1,
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
    require(agent.session.entries.size() == 4 * k_tool_calls + 5, "the long-running task did not retain its complete semantic history");
    provider_mock.verify();
}

void test_tool_output_budget_tracks_model_context() {
    ToolSet tools(std::filesystem::current_path());
    auto observed_budgets = std::make_shared<std::vector<usize>>();
    auto registered = tools.register_tool({
        .definition = {.name = "test_budget", .description = "Observe the tool output budget"},
        .execution_mode = ToolExecutionMode::PARALLEL,
        .execute = [observed_budgets](const ToolSet &, const provider::ToolCall &call,
                                      ToolOutputGrant grant) -> lighter::Task<ToolOutcome, Error> {
            observed_budgets->push_back(grant.payload_bytes);
            co_return ToolOutcome{.call_id = call.id, .payload = std::string(grant.payload_bytes, 'x')};
        },
    });
    require(registered.has_value(), "failed to register the output-budget test tool");

    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .then_calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            glz::generic padded_input;
            const auto encoded = "{\"padding\":\"" + std::string(6'000, 'p') + "\"}";
            require(!glz::read_json(padded_input, encoded), "failed to create padded tool input");
            emit_tool_call(callbacks, "budget-1", "test_budget");
            emit_tool_call(callbacks, "budget-2", "test_budget", std::move(padded_input));
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        })
        .then_calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "small-context", .context_window = 4'000, .max_output_tokens = 500},
    };
    Agent agent(std::move(choice), tools, {});

    lighter::EventLoop loop;
    auto task = agent.run_task("measure", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "context-aware output-budget task failed");
    require(observed_budgets->size() == 2 && std::ranges::all_of(*observed_budgets, [](usize budget) { return budget < 8'000; }),
            "each tool must obtain a fresh allowance that charges the current response and tool arguments");
    const auto result_entry = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "budgeted tool batch did not produce durable results");
    const auto &results = std::get<session::ToolOutcomes>(result_entry->payload).outcomes;
    usize result_bytes = 0;
    for (const auto &result : results) result_bytes += result.payload.size();
    usize committed_bytes = 0;
    for (const auto budget : *observed_budgets) committed_bytes += budget;
    require(results.size() == 2 && result_bytes == committed_bytes &&
                std::ranges::none_of(results, [](const ToolOutcome &outcome) { return tool_outcome_is_error(outcome.kind); }),
            "parallel tools must receive immutable shares of one committed provider-call allowance");
    provider_mock.verify();
}

void test_insufficient_batch_capacity_prevents_tool_dispatch() {
    ToolSet tools(std::filesystem::current_path());
    auto executions = register_noop_tool(tools);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            glz::generic padded_input;
            const auto encoded = "{\"padding\":\"" + std::string(3'450, 'p') + "\"}";
            require(!glz::read_json(padded_input, encoded), "failed to create capacity exhaustion input");
            emit_tool_call(callbacks, "capacity", "test_noop", std::move(padded_input));
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        });
    Agent agent({.handle = provider_mock.handle(),
                 .entry = {.provider = "fake", .id = "tiny-context", .context_window = 1'000, .max_output_tokens = 100}},
                tools, {});

    lighter::EventLoop loop;
    auto task = agent.run_task("measure", {});
    loop.schedule(task);
    loop.run();

    const auto &outcome = task.result();
    require(outcome.has_error() && outcome.error().message().contains("compact first") && *executions == 0,
            "a batch without room for an actionable result must fail before tool dispatch (executions " + std::to_string(*executions) +
                (outcome.has_error() ? ", error " + outcome.error().message() : ", no error") + ")");
    const auto result_entry = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "capacity refusal left an assistant tool call without a matching result");
    const auto &results = std::get<session::ToolOutcomes>(result_entry->payload).outcomes;
    require(results.size() == 1 && results.front().call_id == "capacity" && results.front().kind == ToolOutcomeKind::NOT_STARTED,
            "capacity refusal did not durably close the accepted tool call");
    const auto settled = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        const auto *round = std::get_if<session::ProviderRoundSettled>(&entry.payload);
        return round && round->replay == session::ProviderRoundReplay::OMIT;
    });
    require(settled != agent.session.entries.end(), "capacity refusal did not mark its provisional round audit-only");
    provider_mock.verify();
}

void test_tool_payload_framing_is_reserved_during_planning() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "test_framing", .description = "Test payload framing"},
        .execution_mode = ToolExecutionMode::PARALLEL,
        .receipt_bytes = 200,
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id, .payload = "payload"};
        },
    });
    require(registered.has_value(), "failed to register the payload-framing test tool");

    usize projected_receipt = 0;
    agent::detail::ToolBatchPlanner planner(tools, [&projected_receipt](std::span<const ToolOutcome> outcomes) -> std::optional<usize> {
        require(outcomes.size() == 1, "payload-framing projection omitted a tool outcome");
        projected_receipt = outcomes.front().receipt.size();
        return 32;
    });
    auto admission = planner.plan({provider::ToolCall{.id = "framed", .name = "test_framing"}});
    const auto *plan = std::get_if<agent::detail::ToolBatchPlan>(&admission);
    require(plan && plan->calls.size() == 1 && plan->calls.front().grant.payload_bytes == 32,
            "payload-framing fixture did not receive a nonempty payload grant");
    require(projected_receipt == 200 + k_tool_outcome_payload_framing.size(),
            "planner did not charge fixed payload framing as projected control overhead");
}

void test_tool_error_results_share_the_output_guard() {
    ToolSet tools(std::filesystem::current_path());
    auto invalid_detail = [] {
        auto detail = std::string(128 * 1024, 'e');
        detail[0] = static_cast<char>(0xff);
        return detail;
    };
    auto validation = tools.register_tool({
        .definition = {.name = "test_validation_error"},
        .validate = [invalid_detail](const provider::ToolCall &) -> Result<void> {
            return lighter::outcome_error(Error::tool(invalid_detail()));
        },
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id};
        },
    });
    auto execution = tools.register_tool({
        .definition = {.name = "test_execution_error"},
        .execute = [invalid_detail](const ToolSet &, const provider::ToolCall &, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_await lighter::fail(Error::tool(invalid_detail()));
        },
    });
    require(validation && execution, "failed to register tool error guard fixtures");

    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .then_calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_tool_call(callbacks, "validation", "test_validation_error");
            emit_tool_call(callbacks, "execution", "test_execution_error");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        })
        .then_calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        });
    Agent agent({.handle = provider_mock.handle(),
                 .entry = {.provider = "fake", .id = "bounded-context", .context_window = 20'000, .max_output_tokens = 500}},
                tools, {});

    lighter::EventLoop loop;
    auto task = agent.run_task("exercise errors", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(),
            "bounded tool error task failed" + (task.result().has_error() ? ": " + task.result().error().message() : std::string{}));
    const auto result_entry = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "tool errors were not retained");
    const auto &results = std::get<session::ToolOutcomes>(result_entry->payload).outcomes;
    usize result_bytes = 0;
    for (const auto &result : results) {
        result_bytes += result.payload.size();
        require(tool_outcome_is_error(result.kind) && lighter::encoding::utf8::is_valid(result.payload),
                "every normalized tool error must be bounded valid UTF-8");
    }
    require(results.size() == 2 && result_bytes <= 40'000, "validation and execution errors must share the batch allowance");
    require(results[0].kind == ToolOutcomeKind::FAILED && results[1].kind == ToolOutcomeKind::OUTCOME_UNKNOWN,
            "validation failure and missing execution outcome were not distinguished semantically");
    provider_mock.verify();
}

void test_tool_waits_for_committed_output_allowance() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "test_committed", .description = "Test committed scheduling"},
        .execution_mode = ToolExecutionMode::PARALLEL,
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_return ToolOutcome{.call_id = call.id, .payload = "committed"};
        },
    });
    require(registered.has_value(), "failed to register the committed scheduling test tool");

    auto completions = std::make_shared<usize>(0);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ != 0) {
                emit_message(callbacks, "final", "done", true);
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            }
            emit_message(callbacks, "before", "I will inspect.", true);
            emit_tool_call(callbacks, "committed-call", "test_committed");
            emit_message(callbacks, "after", "The inspection is queued.", true);
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        })
        .times(2);
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("work with committed output", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "committed scheduling task failed");
    usize tool_started = events.size();
    usize tool_completed = events.size();
    usize after_delta = events.size();
    for (usize index = 0; index < events.size(); ++index) {
        if (std::holds_alternative<ToolStarted>(events[index])) tool_started = index;
        if (std::holds_alternative<ToolCompleted>(events[index])) tool_completed = index;
        if (const auto *delta = std::get_if<AssistantTextDelta>(&events[index]); delta && delta->text == "The inspection is queued.") {
            after_delta = index;
        }
    }
    require(after_delta < tool_started && tool_started < tool_completed,
            "tool execution began before the provider batch received a committed output allowance");
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
    require(std::get<session::ProviderCallCompleted>(agent.session.entries[2].payload).loop_outcome ==
                session::ProviderCallLoopOutcome::FAILED,
            "rejected provider completion was not recorded as a failed loop outcome");
    require(std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "missing terminal output did not fail the semantic task");
    provider_mock.verify();
}

void test_failed_completed_round_closes_queued_tool_calls() {
    ToolSet tools(std::filesystem::current_path());
    auto executions = register_noop_tool(tools);
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_tool_call(callbacks, "truncated-call", "test_noop");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::TRUNCATED, .stop_detail = "output_limit"};
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("handle a truncated tool round", {});
    loop.schedule(task);
    loop.run();

    auto task_outcome = task.result();
    require(task_outcome.has_error(), "failed completed provider round did not fail the task");
    require(task_outcome.error().message().contains("response truncated"),
            "failed completed provider round returned the wrong task failure: " + task_outcome.error().message());
    require(*executions == 0, "failed completed provider round dispatched its queued tool");
    const auto outcomes = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(outcomes != agent.session.entries.end(), "failed completed provider round left a dangling tool call");
    const auto &result = std::get<session::ToolOutcomes>(outcomes->payload).outcomes;
    require(result.size() == 1 && result.front().call_id == "truncated-call" && result.front().kind == ToolOutcomeKind::NOT_STARTED &&
                result.front().receipt.contains("provider_round_failed"),
            "failed completed provider round did not record its tool as not started");
    require(agent.session.validate().has_value(), "failed completed provider round left an incoherent durable session");
    auto context = agent.context_manifest();
    require(context && context->provider_history.size() == 3,
            "failed completed provider round leaked audit-only output into resumable context");
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
    require(agent.session.reply_from_latest() == "Done.", "copyable reply included text from the earlier tool-bearing provider call");
    const auto &premature =
        std::get<provider::AssistantMessageItem>(std::get<session::OutputItemCompleted>(agent.session.entries[1].payload).item);
    require(premature.phase == provider::MessagePhase::FINAL && premature.parts[0].text == "I found the answer.",
            "terminal-reply projection changed the provider's original message semantics");
    const auto &first_call = std::get<session::ProviderCallCompleted>(agent.session.entries[3].payload);
    const auto &tool_results = std::get<session::ToolOutcomes>(agent.session.entries[4].payload);
    const auto &terminal_call = std::get<session::ProviderCallCompleted>(agent.session.entries[7].payload);
    require(first_call.loop_outcome == session::ProviderCallLoopOutcome::FOLLOW_UP &&
                terminal_call.loop_outcome == session::ProviderCallLoopOutcome::TERMINAL,
            "provider calls did not record the agent loop's follow-up and terminal decisions");
    require(tool_results.outcomes.size() == 1 && tool_results.outcomes[0].payload == "ok",
            "tool-bearing provider call did not retain its tool result before follow-up");
    provider_mock.verify();
}

void test_cancelled_task_retains_semantic_progress() {
    ToolSet tools(std::filesystem::current_path());
    auto registered = tools.register_tool({
        .definition = {.name = "test_slow", .description = "Test cancellation"},
        .execute = [](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            lighter::Event parked;
            co_await parked.wait();
            co_return ToolOutcome{.call_id = call.id, .payload = "late"};
        },
    });
    require(registered.has_value(), "failed to register the cancellable test tool");

    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_message(callbacks, "progress", "I started the slow work.");
            emit_tool_call(callbacks, "slow-call", "test_slow");
            lighter::Event parked;
            co_await parked.wait();
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::CancellationSource cancellation;
    lighter::EventLoop loop;
    auto task = agent.run_task(
        "start slow work",
        [&cancellation](const Event &event) {
            if (std::holds_alternative<AssistantMessageCompleted>(event)) cancellation.cancel();
        },
        cancellation.token());
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.is_cancelled(), "task cancellation did not propagate to the caller");
    require(agent.session.entries.size() == 7 && std::holds_alternative<session::TaskStarted>(agent.session.entries[0].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[2].payload) &&
                std::get<session::ProviderCallAborted>(agent.session.entries[3].payload).reason ==
                    session::ProviderCallAbortReason::CANCELLED &&
                std::holds_alternative<session::ToolOutcomes>(agent.session.entries[4].payload) &&
                std::get<session::ToolOutcomes>(agent.session.entries[4].payload).outcomes.size() == 1 &&
                std::get<session::ToolOutcomes>(agent.session.entries[4].payload).outcomes[0].call_id == "slow-call" &&
                std::get<session::ToolOutcomes>(agent.session.entries[4].payload).outcomes[0].kind == ToolOutcomeKind::NOT_STARTED &&
                std::get<session::ToolOutcomes>(agent.session.entries[4].payload).outcomes[0].receipt.contains("provider_call_cancelled") &&
                std::get<session::ProviderRoundSettled>(agent.session.entries[5].payload).replay == session::ProviderRoundReplay::OMIT &&
                std::get<session::TaskFinished>(agent.session.entries[6].payload).outcome == session::TaskOutcome::CANCELLED,
            "cancelled task did not retain completed output items and its terminal outcome");
    auto context = agent.context_manifest();
    require(context && context->provider_history.size() == 3 && context->provider_history.back().role == provider::Role::USER,
            "cancelled pre-dispatch provider round leaked incomplete output into resumable context");
    provider_mock.verify();
}

void test_stream_failure_retains_queued_tool_result() {
    ToolSet tools(std::filesystem::current_path());
    auto executions = register_noop_tool(tools);
    const auto diagnostic_prefix_size = Error::protocol("").message().size();
    auto failure = std::make_shared<std::string>(4095 - diagnostic_prefix_size, 'x');
    *failure += "\xF0\x9F\x98\x80 trailing diagnostic";
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [failure](const provider::History &, const std::vector<provider::ToolDefinition> &,
                  const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_tool_call(callbacks, "queued-before-failure", "test_noop");
            co_await lighter::fail(Error::protocol(*failure));
        });
    provider_mock.expect<provider::CompactDispatch>().never();

    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("do fragile work", {});
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.has_error() && outcome.error().detail == *failure && *executions == 0,
            "provider stream failure dispatched a queued tool or did not reach the caller");
    const auto &durable_failure = std::get<session::ProviderCallAborted>(agent.session.entries[2].payload).detail;
    require(durable_failure.size() == 4095 && lighter::encoding::utf8::is_valid(durable_failure),
            "durable provider diagnostic split a UTF-8 code point at its byte bound");
    require(agent.session.entries.size() == 6 && std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[1].payload) &&
                std::get<session::ProviderCallAborted>(agent.session.entries[2].payload).reason ==
                    session::ProviderCallAbortReason::FAILED &&
                std::holds_alternative<session::ToolOutcomes>(agent.session.entries[3].payload) &&
                std::get<session::ToolOutcomes>(agent.session.entries[3].payload).outcomes.front().call_id == "queued-before-failure" &&
                std::get<session::ProviderRoundSettled>(agent.session.entries[4].payload).replay == session::ProviderRoundReplay::OMIT &&
                std::get<session::TaskFinished>(agent.session.entries[5].payload).outcome == session::TaskOutcome::FAILED,
            "stream failure lost the queued tool call or its synthetic result");
    provider_mock.verify();
}

void test_invalid_tool_is_rejected_before_dispatch() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                emit_tool_call(callbacks, "unknown-call", "missing_tool");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            }
            require(history.back().role == provider::Role::USER && history.back().parts.size() == 1,
                    "invalid tool call did not produce a model-visible result");
            const auto *result = std::get_if<ToolOutcome>(&history.back().parts[0]);
            require(result && result->call_id == "unknown-call" && result->kind == ToolOutcomeKind::FAILED &&
                        result->payload.contains("unknown tool"),
                    "invalid tool result did not describe the pre-dispatch validation failure");
            emit_message(callbacks, "done", "Recovered.");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    provider_mock.expect<provider::CompactDispatch>().never();

    std::vector<Event> events;
    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools);
    lighter::EventLoop loop;
    auto task = agent.run_task("use an invalid tool", [&events](const Event &event) { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "agent did not recover from a model-visible invalid tool result");
    require(std::ranges::none_of(events, [](const Event &event) { return std::holds_alternative<ToolStarted>(event); }),
            "invalid tool emitted ToolStarted before lookup and input validation");
    provider_mock.verify();
}

struct ToolSchedulingProbe {
    usize running = 0;
    usize max_running = 0;
    bool exclusive_overlapped = false;
    bool exclusive_finished = false;
    bool trailing_started_early = false;
    lighter::Event parallel_wave_started;
};

void test_tool_execution_semantics() {
    constexpr usize k_parallel_calls = 40;
    ToolSet tools(std::filesystem::current_path());
    auto probe = std::make_shared<ToolSchedulingProbe>();
    auto parallel = tools.register_tool({
        .definition = {.name = "test_parallel", .description = "Test parallel scheduling"},
        .execution_mode = ToolExecutionMode::PARALLEL,
        .execute = [probe](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            if (call.id == "trailing") {
                if (!probe->exclusive_finished) probe->trailing_started_early = true;
                co_return ToolOutcome{.call_id = call.id, .payload = "parallel"};
            }
            ++probe->running;
            probe->max_running = std::max(probe->max_running, probe->running);
            if (probe->running == k_parallel_calls) probe->parallel_wave_started.set();
            co_await probe->parallel_wave_started.wait();
            --probe->running;
            co_return ToolOutcome{.call_id = call.id, .payload = "parallel"};
        },
    });
    require(parallel.has_value(), "failed to register the parallel scheduling test tool");
    auto exclusive = tools.register_tool({
        .definition = {.name = "test_exclusive", .description = "Test exclusive scheduling"},
        .execute = [probe](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            probe->exclusive_overlapped = probe->running != 0;
            probe->exclusive_finished = true;
            co_return ToolOutcome{.call_id = call.id, .payload = "exclusive"};
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
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "tool scheduling task did not retain its result batch");
    const auto &results = std::get<session::ToolOutcomes>(result_entry->payload).outcomes;
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
    require(events.size() == 4 && std::holds_alternative<AssistantMessageCompleted>(events[0]) &&
                std::holds_alternative<ProviderActivityCompleted>(events[1]) && std::holds_alternative<SessionNotice>(events[2]) &&
                std::holds_alternative<TaskCompleted>(events[3]),
            "automatic compaction emitted the wrong lifecycle events");
    require(agent.session.entries.size() == 11 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[7].payload) &&
                std::holds_alternative<session::TaskFinished>(agent.session.entries[10].payload),
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
    require(agent.session.entries.size() == 16 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[11].payload),
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
    require(agent.session.entries.size() == 11 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload),
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
    require(agent.session.entries.size() == 10 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload) &&
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
    session::Session session_log;
    session_log.start_task("change the project");
    const auto session_id = session_log.id;

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
    require(manifest->session_id == session_id && manifest->session_entries.size() == 1 && manifest->session_entries[0].value == 1,
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
    test_tool_output_budget_tracks_model_context();
    test_insufficient_batch_capacity_prevents_tool_dispatch();
    test_tool_payload_framing_is_reserved_during_planning();
    test_tool_error_results_share_the_output_guard();
    test_tool_waits_for_committed_output_allowance();
    test_done_requires_terminal_answer();
    test_failed_completed_round_closes_queued_tool_calls();
    test_final_phase_does_not_bypass_tool_follow_up();
    test_cancelled_task_retains_semantic_progress();
    test_stream_failure_retains_queued_tool_result();
    test_invalid_tool_is_rejected_before_dispatch();
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
