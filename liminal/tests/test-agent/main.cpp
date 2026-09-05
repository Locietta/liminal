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
#include <liminal/provider/compact.h>
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
    const auto provider_call_id = log.next_provider_call();
    log.append(session::OutputItemCompleted{
        .task_id = task_id,
        .provider_call_id = provider_call_id,
        .item = provider::AssistantMessageItem{.id = {.value = "seed"}, .parts = {{.text = std::move(text)}}},
    });
    log.append(session::ProviderCallCompleted{
        .task_id = task_id,
        .id = provider_call_id,
        .completion = {.usage = usage},
        .loop_outcome = session::ProviderCallLoopOutcome::TERMINAL,
    });
    log.append(session::ProviderRoundSettled{
        .task_id = task_id,
        .provider_call_id = provider_call_id,
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
    provider_mock.expect<provider::CompactDispatch>().calls([](provider::History &history,
                                                               provider::CompactRequest request) -> lighter::Task<void, Error> {
        require(request.instructions == "keep decisions" && request.retained_prompt_tokens == provider::k_default_retained_prompt_tokens,
                "compaction received the wrong instructions or retention budget");
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
    EventSink sink = [&events](const Event &event) noexcept { events.push_back(event); };

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
    // Tools that spend their whole allowance leave the context at the
    // compaction threshold, so the follow-up call compacts first.
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            co_return;
        });
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

void test_throwing_tool_settles_its_batch() {

    ToolSet tools(std::filesystem::current_path());
    auto throwing = tools.register_tool({
        .definition = {.name = "test_throwing"},
        .execute = [](const ToolSet &, const provider::ToolCall &, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
            co_await lighter::yield();
            throw std::runtime_error("tool blew up");
        },
    });
    auto executions = register_noop_tool(tools);
    require(throwing.has_value(), "failed to register the throwing tool");

    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>()
        .then_calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_tool_call(callbacks, "boom", "test_throwing");
            emit_tool_call(callbacks, "after", "test_noop");
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

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("exercise a throwing tool", [&events](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a throwing tool must not hang or fail the task");
    require(*executions == 1, "the tool queued behind a throwing tool must still run");
    const auto boom_started = std::ranges::any_of(events, [](const Event &event) {
        const auto *started = std::get_if<ToolStarted>(&event);
        return started && started->call_id == "boom";
    });
    const auto boom_completed = std::ranges::find_if(events, [](const Event &event) {
        const auto *completed = std::get_if<ToolCompleted>(&event);
        return completed && completed->call_id == "boom";
    });
    require(boom_started && boom_completed != events.end() && std::get<ToolCompleted>(*boom_completed).is_error,
            "the display layer must see the throwing call start and then complete as a failure");
    const auto result_entry = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
        return std::holds_alternative<session::ToolOutcomes>(entry.payload);
    });
    require(result_entry != agent.session.entries.end(), "tool outcomes were not retained");
    const auto &results = std::get<session::ToolOutcomes>(result_entry->payload).outcomes;
    require(results.size() == 2, "both tool calls must settle");
    require(results[0].call_id == "boom" && results[0].kind == ToolOutcomeKind::OUTCOME_UNKNOWN &&
                results[0].payload.contains("tool blew up"),
            "a throwing tool must settle as an execution error carrying the exception message");
    require(results[1].call_id == "after" && !tool_outcome_is_error(results[1].kind), "the following tool must complete normally");
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
    auto task = agent.run_task("work with committed output", [&events](const Event &event) noexcept { events.push_back(event); });
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
        [&cancellation](const Event &event) noexcept {
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
    auto task = agent.run_task("use an invalid tool", [&events](const Event &event) noexcept { events.push_back(event); });
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

/// Records the summarization request so the test can inspect what reaches
/// the wire. Conforms to ProviderFacade without going through the mock, which
/// keeps the check on the request shape rather than on dispatch order.
struct RecordingSummaryProvider {
    provider::History seen;
    usize tool_definitions = 0;

    lighter::Task<provider::ProviderCallCompletion, Error> complete(const provider::History &history,
                                                                    const std::vector<provider::ToolDefinition> &tools,
                                                                    const provider::StreamCallbacks &callbacks) {
        seen = history;
        tool_definitions = tools.size();
        emit_message(callbacks, "summary", "SUMMARY");
        co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
    }

    lighter::Task<void, Error> compact(provider::History &, provider::CompactRequest) { co_return; }
};

void test_local_compaction_summarizes_tool_rounds_as_text() {

    provider::History history;
    provider::append_developer(history, "policy");
    provider::append_user(history, "first task");
    glz::generic input;
    require(!glz::read_json(input, R"({"path":"README.md"})"), "failed to build tool input");
    history.push_back(
        {.role = provider::Role::ASSISTANT, .parts = {provider::ToolCall{.id = "call-1", .name = "read_file", .input = std::move(input)}}});
    provider::append_tool_outcomes(history, {ToolOutcome{.call_id = "call-1", .receipt = "continuation: none", .payload = "contents"}});
    history.push_back({.role = provider::Role::ASSISTANT, .parts = {provider::OpaquePart{.provider_tag = "fake", .payload = "{}"}}});
    history.push_back({.role = provider::Role::ASSISTANT, .parts = {provider::TextPart{.text = "done with the first task"}}});
    provider::append_user(history, "second task");
    provider::append_output_item(history, provider::AssistantMessageItem{.id = {.value = "answer"}, .parts = {{.text = "answer"}}});

    RecordingSummaryProvider provider;
    lighter::EventLoop loop;
    auto task = provider::local_compact(&provider, history, {});
    loop.schedule(task);
    loop.run();
    require(task.result().has_value(), "local compaction failed");

    require(provider.tool_definitions == 0, "the summarization request must not advertise tools");
    require(provider.seen.size() == 8, "the summarization request kept the wrong number of items: " + std::to_string(provider.seen.size()));
    for (const auto &item : provider.seen) {
        for (const auto &part : item.parts) {
            require(std::holds_alternative<provider::TextPart>(part),
                    "every part of the summarization request must be plain text so providers accept it without tools");
        }
    }
    const auto call_text = std::get<provider::TextPart>(provider.seen[2].parts[0]).text;
    const auto result_text = std::get<provider::TextPart>(provider.seen[3].parts[0]).text;
    require(call_text.contains("read_file") && call_text.contains("README.md"), "the rendered tool call lost its name or arguments");
    require(result_text.contains("call-1") && result_text.contains("contents"), "the rendered tool result lost its payload");
    require(provider.seen.back().role == provider::Role::USER &&
                std::get<provider::TextPart>(provider.seen.back().parts[0]).text.contains("handoff summary"),
            "the summarization prompt must be the final user item");

    require(std::get<provider::TextPart>(provider.seen[6].parts[0]).text == "answer",
            "the summarization request must cover the latest task as well");
    require(history.size() == 4 && history[0].role == provider::Role::DEVELOPER &&
                std::get<provider::TextPart>(history[1].parts[0]).text == "first task" &&
                std::get<provider::TextPart>(history[2].parts[0]).text == "second task" &&
                std::get<provider::TextPart>(history[3].parts[0]).text.starts_with("[Compacted context]") &&
                std::get<provider::TextPart>(history[3].parts[0]).text.contains("SUMMARY"),
            "compaction must keep the instruction prefix and recent prompts, then end with the summary");
}

void test_local_compaction_bounds_retained_prompts() {

    provider::History history;
    provider::append_developer(history, "policy");
    provider::append_user(history, "[Compacted context] an earlier handoff\n\nOLD SUMMARY");
    provider::append_user(history, std::string(70'000, 'a'));
    provider::append_output_item(history, provider::AssistantMessageItem{.id = {.value = "a1"}, .parts = {{.text = "working"}}});
    provider::append_tool_outcomes(history, {ToolOutcome{.call_id = "call-1", .payload = "contents"}});
    provider::append_user(history, std::string(30'000, 'b'));
    provider::append_user(history, "latest");

    RecordingSummaryProvider provider;
    lighter::EventLoop loop;
    auto task = provider::local_compact(&provider, history, {});
    loop.schedule(task);
    loop.run();
    require(task.result().has_value(), "local compaction failed");

    require(provider.seen.size() == 8, "the summarization request must cover every conversation item");
    // 20k retained tokens: "latest" (2 + 6 framing) and the 30k-byte prompt
    // (7'500 + 6) fit whole; the 70k-byte prompt is truncated to the
    // remaining 12'486 tokens less its framing.
    require(history.size() == 5 && std::get<provider::TextPart>(history[1].parts[0]).text == std::string(49'920, 'a') &&
                std::get<provider::TextPart>(history[2].parts[0]).text == std::string(30'000, 'b') &&
                std::get<provider::TextPart>(history[3].parts[0]).text == "latest" &&
                std::get<provider::TextPart>(history[4].parts[0]).text.contains("SUMMARY"),
            "retained prompts must be the newest real prompts within the token budget, oldest truncated, summary last");
    for (const auto &item : history) {
        for (const auto &part : item.parts) {
            require(!std::get<provider::TextPart>(part).text.contains("OLD SUMMARY"),
                    "an earlier handoff summary and tool rounds must not be retained as prompts");
        }
    }

    // A caller-supplied budget bounds retention the same way: 100 tokens keep
    // "latest" (8 with framing) and 344 bytes of the newest large prompt.
    provider::History small;
    provider::append_developer(small, "policy");
    provider::append_user(small, std::string(30'000, 'b'));
    provider::append_user(small, "latest");
    lighter::EventLoop small_loop;
    auto small_task = provider::local_compact(&provider, small, {.retained_prompt_tokens = 100});
    small_loop.schedule(small_task);
    small_loop.run();
    require(small_task.result().has_value(), "local compaction with a small retention budget failed");
    require(small.size() == 4 && std::get<provider::TextPart>(small[1].parts[0]).text == std::string(344, 'b') &&
                std::get<provider::TextPart>(small[2].parts[0]).text == "latest" &&
                std::get<provider::TextPart>(small[3].parts[0]).text.contains("SUMMARY"),
            "a small retention budget must truncate the retained prompts accordingly");

    // A target for the compacted history as a whole is applied once the
    // summary is known: instructions and summary come first, prompts absorb
    // the rest and vanish entirely when nothing is left. The target is
    // best-effort: it cannot shrink the instructions or the summary.
    provider::History bounded;
    provider::append_developer(bounded, "policy");
    provider::append_user(bounded, std::string(30'000, 'b'));
    provider::append_user(bounded, "latest");
    lighter::EventLoop bounded_loop;
    auto bounded_task = provider::local_compact(&provider, bounded, {.compacted_context_target_tokens = 200});
    bounded_loop.schedule(bounded_task);
    bounded_loop.run();
    require(bounded_task.result().has_value(), "local compaction with a compacted-context target failed");
    const auto &bounded_prompt = std::get<provider::TextPart>(bounded[1].parts[0]).text;
    require(bounded.size() == 4 && !bounded_prompt.empty() && bounded_prompt.size() < 30'000 &&
                std::get<provider::TextPart>(bounded[2].parts[0]).text == "latest" &&
                std::get<provider::TextPart>(bounded[3].parts[0]).text.contains("SUMMARY"),
            "the compacted-context target must shrink retention below the caller's prompt cap");

    provider::History tight;
    provider::append_developer(tight, "policy");
    provider::append_user(tight, std::string(30'000, 'b'));
    provider::append_user(tight, "latest");
    lighter::EventLoop tight_loop;
    auto tight_task = provider::local_compact(&provider, tight, {.compacted_context_target_tokens = 10});
    tight_loop.schedule(tight_task);
    tight_loop.run();
    require(tight_task.result().has_value(), "local compaction with an unreachable compacted-context target failed");
    require(tight.size() == 2 && tight[0].role == provider::Role::DEVELOPER &&
                std::get<provider::TextPart>(tight[1].parts[0]).text.contains("SUMMARY"),
            "when instructions and summary already exceed the target, no prompts may be retained");
}

/// Rejects summarization requests above an item limit the way a provider
/// with an exhausted context does, alternating between a classified API
/// error and a CONTEXT_EXHAUSTED stop so both rejection shapes are covered.
struct OverflowingSummaryProvider {
    usize item_limit = 0;
    std::vector<usize> attempt_sizes;
    provider::History accepted;

    lighter::Task<provider::ProviderCallCompletion, Error>
    complete(const provider::History &history, const std::vector<provider::ToolDefinition> &, const provider::StreamCallbacks &callbacks) {
        attempt_sizes.push_back(history.size());
        if (history.size() > item_limit) {
            if (attempt_sizes.size() % 2 == 1) {
                auto error = Error::http_status(400, "invalid_request_error", "prompt is too long", "req_long");
                error.code = ErrorCode::CONTEXT_EXHAUSTED;
                co_await lighter::fail(std::move(error));
            }
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::CONTEXT_EXHAUSTED,
                                                       .stop_detail = "model_context_window_exceeded"};
        }
        accepted = history;
        emit_message(callbacks, "summary", "SUMMARY");
        co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
    }

    lighter::Task<void, Error> compact(provider::History &, provider::CompactRequest) { co_return; }
};

void test_local_compaction_drops_oldest_items_in_batches_when_the_summary_request_overflows() {

    provider::History history;
    provider::append_developer(history, "policy");
    provider::append_user(history, "one");
    provider::append_output_item(history, provider::AssistantMessageItem{.id = {.value = "a1"}, .parts = {{.text = "reply one"}}});
    provider::append_user(history, "two");
    provider::append_output_item(history, provider::AssistantMessageItem{.id = {.value = "a2"}, .parts = {{.text = "reply two"}}});
    provider::append_user(history, "three");

    OverflowingSummaryProvider provider{.item_limit = 4};
    lighter::EventLoop loop;
    auto task = provider::local_compact(&provider, history, {});
    loop.schedule(task);
    loop.run();
    require(task.result().has_value(), "local compaction did not recover from an oversized summarization request");

    require(provider.attempt_sizes == std::vector<usize>{7, 6, 4},
            "retries must drop the oldest conversation items in doubling batches from the summarization request");
    require(provider.accepted.size() == 4 && provider.accepted[0].role == provider::Role::DEVELOPER &&
                std::get<provider::TextPart>(provider.accepted[1].parts[0]).text == "reply two" &&
                std::get<provider::TextPart>(provider.accepted[2].parts[0]).text == "three",
            "the accepted summarization request must keep the instruction prefix and the newest items");
    require(history.size() == 5 && std::get<provider::TextPart>(history[1].parts[0]).text == "one" &&
                std::get<provider::TextPart>(history[3].parts[0]).text == "three" &&
                std::get<provider::TextPart>(history[4].parts[0]).text.contains("SUMMARY"),
            "prompt retention must draw on the full transcript, not on the trimmed summarization request");

    // Even with room for one more item, probing stops at the first accepted
    // cut: every accepted probe generates a billable summary, so no request
    // is made after acceptance.
    provider::History refined;
    provider::append_developer(refined, "policy");
    provider::append_user(refined, "one");
    provider::append_output_item(refined, provider::AssistantMessageItem{.id = {.value = "a1"}, .parts = {{.text = "reply one"}}});
    provider::append_user(refined, "two");
    provider::append_output_item(refined, provider::AssistantMessageItem{.id = {.value = "a2"}, .parts = {{.text = "reply two"}}});
    provider::append_user(refined, "three");
    OverflowingSummaryProvider refining{.item_limit = 5};
    lighter::EventLoop refined_loop;
    auto refined_task = provider::local_compact(&refining, refined, {});
    refined_loop.schedule(refined_task);
    refined_loop.run();
    require(refined_task.result().has_value(), "local compaction with room for a tighter cut failed");
    require(refining.attempt_sizes == std::vector<usize>{7, 6, 4} && refining.accepted.size() == 4 &&
                std::get<provider::TextPart>(refining.accepted[1].parts[0]).text == "reply two",
            "probing must stop at the first accepted cut instead of generating further summaries");

    // A request bound trims by estimate before the first attempt: three
    // 4'000-byte items (about 1'006 tokens each) against a 1'500-token bound
    // leave only the newest one, with no rejected request at all.
    provider::History estimated;
    provider::append_developer(estimated, "policy");
    provider::append_user(estimated, std::string(4'000, 'a'));
    provider::append_output_item(estimated,
                                 provider::AssistantMessageItem{.id = {.value = "b"}, .parts = {{.text = std::string(4'000, 'b')}}});
    provider::append_user(estimated, std::string(4'000, 'c'));
    OverflowingSummaryProvider roomy{.item_limit = 10};
    lighter::EventLoop estimated_loop;
    auto estimated_task = provider::local_compact(&roomy, estimated, {.summary_request_tokens = 1'500});
    estimated_loop.schedule(estimated_task);
    estimated_loop.run();
    require(estimated_task.result().has_value(), "local compaction with a request bound failed");
    require(roomy.attempt_sizes == std::vector<usize>{3} && roomy.accepted.size() == 3 &&
                std::get<provider::TextPart>(roomy.accepted[1].parts[0]).text == std::string(4'000, 'c'),
            "the request bound must leave out the oldest items by estimate before the first attempt");
    require(estimated.size() == 4 && std::get<provider::TextPart>(estimated[1].parts[0]).text == std::string(4'000, 'a') &&
                std::get<provider::TextPart>(estimated[2].parts[0]).text == std::string(4'000, 'c'),
            "prompt retention must still draw on the full transcript when the request was trimmed by estimate");

    // Provider-private parts never reach the summarizer, so the estimate must
    // not charge them either: a 40'000-byte opaque item would otherwise push
    // the older prompt out of a request it actually fits.
    provider::History opaque;
    provider::append_developer(opaque, "policy");
    provider::append_user(opaque, std::string(4'000, 'a'));
    opaque.push_back(
        {.role = provider::Role::ASSISTANT, .parts = {provider::OpaquePart{.provider_tag = "fake", .payload = std::string(40'000, 'o')}}});
    provider::append_user(opaque, std::string(4'000, 'c'));
    OverflowingSummaryProvider roomy_opaque{.item_limit = 10};
    lighter::EventLoop opaque_loop;
    auto opaque_task = provider::local_compact(&roomy_opaque, opaque, {.summary_request_tokens = 2'500});
    opaque_loop.schedule(opaque_task);
    opaque_loop.run();
    require(opaque_task.result().has_value(), "local compaction with an opaque item failed");
    require(roomy_opaque.attempt_sizes == std::vector<usize>{4} && roomy_opaque.accepted.size() == 4 &&
                std::get<provider::TextPart>(roomy_opaque.accepted[1].parts[0]).text == std::string(4'000, 'a'),
            "the request estimate must ignore provider-private parts that are never sent");

    provider::History single;
    provider::append_developer(single, "policy");
    provider::append_user(single, "only");
    OverflowingSummaryProvider hopeless{.item_limit = 0};
    lighter::EventLoop hopeless_loop;
    auto hopeless_task = provider::local_compact(&hopeless, single, {});
    hopeless_loop.schedule(hopeless_task);
    hopeless_loop.run();
    require(hopeless_task.result().has_error() && hopeless_task.result().error().code == ErrorCode::CONTEXT_EXHAUSTED &&
                hopeless.attempt_sizes.size() == 1 && single.size() == 2,
            "a request that cannot shrink further must surface the provider rejection and leave the history unchanged");
}

void test_automatic_compaction() {

    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest request) -> lighter::Task<void, Error> {
            require(request.instructions.contains("active task"), "automatic compaction received the wrong instructions");
            require(history.size() == 3, "automatic compaction did not receive the accumulated history without the pending prompt");
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
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
    auto task = agent.run_task("latest", [&events](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "automatic compaction task failed");
    require(agent.session.validate().has_value(), "automatic compaction produced a session that does not validate");
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
        .calls([compactions](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
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
            require(history.size() == (completion == 0 ? 3 : 2) && history[0].role == provider::Role::DEVELOPER &&
                        history[1].role == provider::Role::USER,
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
    auto task = agent.run_task("latest", [&events](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a task requiring multiple automatic compactions failed");
    require(agent.session.validate().has_value(), "mid-task automatic compaction produced a session that does not validate");
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
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest request) -> lighter::Task<void, Error> {
            require(history.size() == 3, "proactive compaction did not receive the accumulated context");
            // A tenth of the 500-token input budget.
            require(request.retained_prompt_tokens == 50 && request.compacted_context_target_tokens == 250 &&
                        request.summary_request_tokens == 500,
                    "automatic compaction did not derive its retention, compacted-context, and request bounds from the input budget");
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
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
    // The reserved output halves the input budget: accumulated context
    // reaches the maintenance threshold before the new prompt is charged.
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 500},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, "old", "answer", {.input_tokens = 400, .output_tokens = 50, .context_tokens = 450});
    auto manifest = agent.context_manifest();
    require(manifest && manifest->usage.input_budget_tokens == 500 && manifest->usage.reported_context_tokens == 450,
            "test setup did not produce the intended budget and baseline");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "proactive automatic compaction task failed");
    require(agent.session.entries.size() == 11 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload),
            "90 percent input budget usage did not create an automatic checkpoint");
    provider_mock.verify();
}

void test_estimated_context_triggers_compaction_without_reported_usage() {

    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            require(history.size() == 2, "estimate-triggered compaction did not receive the accumulated context");
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            co_return;
        });
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &history, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY",
                    "estimate-triggered compaction was not used for completion");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    // A checkpoint carries no provider usage, so only the byte estimate can
    // notice that it fills the window while still fitting the input budget.
    session::ContextCheckpoint oversized;
    oversized.items.push_back(session::ContextInput{.parts = {provider::TextPart{.text = std::string(3'600, 's')}}});
    agent.session.append(std::move(oversized));
    auto manifest = agent.context_manifest();
    require(manifest && !manifest->usage.reported_context_tokens && manifest->omitted_budget_entries == 0 &&
                *manifest->usage.remaining_input_tokens > 0,
            "test setup must fit the budget without a reported usage baseline");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "estimate-triggered compaction task failed");
    require(agent.session.entries.size() == 7 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[2].payload),
            "estimated context usage did not create an automatic checkpoint");
    provider_mock.verify();
}

void test_ineffective_automatic_compaction_fails_the_task() {

    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &, provider::CompactRequest) -> lighter::Task<void, Error> { co_return; });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    session::ContextCheckpoint oversized;
    oversized.items.push_back(session::ContextInput{.parts = {provider::TextPart{.text = std::string(3'600, 's')}}});
    agent.session.append(std::move(oversized));

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().detail.contains("did not reduce context"),
            "a compaction that left the context unchanged was not reported");
    require(std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "the ineffective compaction did not fail its task");
    // The unusable checkpoint must not have been committed: the session keeps
    // its original history rather than hiding it behind a summary that does
    // not fit.
    require(agent.session.entries.size() == 3 &&
                std::ranges::count_if(agent.session.entries,
                                      [](const session::SessionEntry &entry) {
                                          return std::holds_alternative<session::ContextCheckpoint>(entry.payload);
                                      }) == 1,
            "an ineffective checkpoint was committed to the append-only session");
    provider_mock.verify();
}

/// Compacts into one assistant item that interleaves text, an opaque part,
/// and text. Checkpoint normalization splits it into three outputs, and the
/// replay charges each its own framing (4 per item, 2 per part): the raw
/// history costs 16 framing tokens, its replay 24.
void compact_into_interleaved_item(provider::History &history, usize opaque_bytes) {
    history.resize(provider::instruction_prefix_size(history));
    history.push_back({.role = provider::Role::ASSISTANT,
                       .parts = {provider::TextPart{.text = std::string(2'000, 'a')},
                                 provider::OpaquePart{.provider_tag = "fake", .payload = std::string(opaque_bytes, 'o')},
                                 provider::TextPart{.text = std::string(1'000, 'b')}}});
}

void test_checkpoint_is_judged_on_its_replay() {
    // A 900-token budget admits the raw compacted history plus the pending
    // prompt, but its normalized replay adds enough framing to overflow.
    {
        ToolSet tools(std::filesystem::current_path());
        lighter::mock::Mock<provider::ProviderFacade> provider_mock;
        provider_mock.expect<provider::CompactDispatch>().calls(
            [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
                compact_into_interleaved_item(history, 465);
                co_return;
            });
        model::Choice choice{
            .handle = provider_mock.handle(),
            .entry = {.provider = "fake", .id = "test", .context_window = 910, .max_output_tokens = 10},
        };
        Agent agent(std::move(choice), tools, compact_test_instructions());
        append_session_message(agent.session, std::string(3'600, 'u'), "answer");

        lighter::EventLoop loop;
        auto task = agent.run_task("latest", {});
        loop.schedule(task);
        loop.run();
        auto outcome = task.result();

        require(outcome.has_error() && outcome.error().detail.contains("exceed the model input budget"),
                "a checkpoint whose replay exceeds the budget was accepted on the strength of its raw history");
        require(std::ranges::none_of(
                    agent.session.entries,
                    [](const session::SessionEntry &entry) { return std::holds_alternative<session::ContextCheckpoint>(entry.payload); }),
                "the over-budget checkpoint was committed");
        provider_mock.verify();
    }
    // Less opaque data fits the budget even above the proactive threshold.
    // The pending prompt follows the three normalized outputs.
    {
        ToolSet tools(std::filesystem::current_path());
        lighter::mock::Mock<provider::ProviderFacade> provider_mock;
        provider_mock.expect<provider::CompactDispatch>().calls(
            [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
                compact_into_interleaved_item(history, 400);
                co_return;
            });
        provider_mock.expect<provider::CompleteDispatch>().calls(
            [](const provider::History &history, const std::vector<provider::ToolDefinition> &,
               const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
                require(history.size() == 5 && history[1].role == provider::Role::ASSISTANT && history[3].role == provider::Role::ASSISTANT,
                        "the replayed checkpoint did not carry the split assistant outputs");
                emit_message(callbacks, "done", "done");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            });
        model::Choice choice{
            .handle = provider_mock.handle(),
            .entry = {.provider = "fake", .id = "test", .context_window = 910, .max_output_tokens = 10},
        };
        Agent agent(std::move(choice), tools, compact_test_instructions());
        append_session_message(agent.session, std::string(3'600, 'u'), "answer");

        lighter::EventLoop loop;
        auto task = agent.run_task("latest", {});
        loop.schedule(task);
        loop.run();

        require(task.result().has_value(), "a checkpoint whose replay fits the budget was rejected");
        const auto committed = std::ranges::find_if(agent.session.entries, [](const session::SessionEntry &entry) {
            return std::holds_alternative<session::ContextCheckpoint>(entry.payload);
        });
        require(committed != agent.session.entries.end() && std::get<session::ContextCheckpoint>(committed->payload).items.size() == 4,
                "the committed checkpoint did not keep its three normalized outputs and pending prompt");
        provider_mock.verify();
    }
}

void test_oversized_prompt_is_rejected_before_any_provider_call() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 80, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());

    lighter::EventLoop loop;
    // The prompt alone exceeds the input budget and a summary could not shrink it.
    auto task = agent.run_task(std::string(1'000, 'p'), {});
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().detail.contains("exceeds the model input budget"),
            "a lone oversized prompt must be rejected without a provider call");
    require(agent.session.entries.size() == 2 &&
                std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "the rejected prompt did not fail its task cleanly");
    provider_mock.verify();
}

/// A provider whose compaction is the real local summarizer, so the agent
/// path is exercised end to end rather than through a mock that rewrites
/// history by hand. Summarization requests answer "SUMMARY"; everything else
/// answers "done" and is recorded for inspection.
struct LocalCompactingProvider {
    std::shared_ptr<std::vector<provider::History>> completions = std::make_shared<std::vector<provider::History>>();

    lighter::Task<provider::ProviderCallCompletion, Error>
    complete(const provider::History &history, const std::vector<provider::ToolDefinition> &, const provider::StreamCallbacks &callbacks) {
        const auto *last = std::get_if<provider::TextPart>(&history.back().parts.back());
        if (last && last->text.contains("context checkpoint compaction")) {
            emit_message(callbacks, "summary", "SUMMARY");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        }
        completions->push_back(history);
        emit_message(callbacks, "done", "done");
        co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
    }

    lighter::Task<void, Error> compact(provider::History &history, provider::CompactRequest request) {
        co_return co_await provider::local_compact(this, history, request).or_fail();
    }
};

void test_automatic_compaction_through_the_local_summarizer() {
    ToolSet tools(std::filesystem::current_path());
    LocalCompactingProvider provider;
    auto completions = provider.completions;
    model::Choice choice{
        .handle = pro::make_proxy<provider::ProviderFacade, LocalCompactingProvider>(std::move(provider)),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    // The finished task no longer fits the 990-token budget next to a new prompt.
    append_session_message(agent.session, std::string(3'600, 'u'), std::string(100, 'a'));

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "automatic compaction through the local summarizer failed");
    require(agent.session.validate().has_value(), "local summarizer compaction produced a session that does not validate");
    // Only the old prompt is subject to retention (99 tokens including
    // framing). The pending prompt follows the handoff unchanged.
    require(completions->size() == 1 && completions->front().size() == 4 &&
                std::get<provider::TextPart>(completions->front()[1].parts[0]).text == std::string(372, 'u') &&
                std::get<provider::TextPart>(completions->front()[3].parts[0]).text == "latest" &&
                std::get<provider::TextPart>(completions->front()[2].parts[0]).text.contains("SUMMARY"),
            "the completion did not see the retained prompts and summary produced by the local summarizer");
}

struct BoundedCompactionState {
    std::vector<provider::History> summaries;
    std::vector<provider::History> requests;
};

/// Uses real local compaction and rejects oversized requests, including
/// summarization overhead. Pending prompts must never reach the summarizer.
struct BoundedCompactingProvider {
    std::shared_ptr<BoundedCompactionState> state;

    lighter::Task<provider::ProviderCallCompletion, Error>
    complete(const provider::History &history, const std::vector<provider::ToolDefinition> &, const provider::StreamCallbacks &callbacks) {
        const auto &text = std::get<provider::TextPart>(history.back().parts.back()).text;
        const bool summary = text.contains("context checkpoint compaction");
        (summary ? state->summaries : state->requests).push_back(history);
        if (summary) {
            for (const auto &item : history) {
                for (const auto &part : item.parts) {
                    require(!std::get<provider::TextPart>(part).text.contains("PENDING PROMPT"),
                            "local compaction received the unprocessed prompt");
                }
            }
        }
        if (context::estimate_input_tokens(history) > 990) {
            auto error = Error::api("context_length_exceeded", "request exceeds input budget", "req_bounded", false);
            error.code = ErrorCode::CONTEXT_EXHAUSTED;
            co_await lighter::fail(std::move(error));
        }
        emit_message(callbacks, summary ? "summary" : "done", summary ? "SUMMARY" : "done");
        co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
    }

    lighter::Task<void, Error> compact(provider::History &history, provider::CompactRequest request) {
        co_return co_await provider::local_compact(this, history, request).or_fail();
    }
};

void test_pending_prompt_admission_with_bounded_local_compaction() {
    for (const usize history_size : {usize{0}, usize{1}, usize{1'000}}) {
        for (const usize prompt_size : {usize{3'600}, usize{3'900}}) {
            ToolSet tools(std::filesystem::current_path());
            auto state = std::make_shared<BoundedCompactionState>();
            model::Choice choice{
                .handle = pro::make_proxy<provider::ProviderFacade, BoundedCompactingProvider>(BoundedCompactingProvider{state}),
                .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
            };
            Agent agent(std::move(choice), tools, compact_test_instructions());
            if (history_size != 0) append_session_message(agent.session, std::string(history_size, 'u'), std::string(history_size, 'a'));
            std::string prompt = "PENDING PROMPT";
            prompt.resize(prompt_size, 'p');
            provider::History fresh;
            provider::append_developer(fresh, "test policy");
            provider::append_user(fresh, prompt);
            const auto input_tokens = context::estimate_input_tokens(fresh);
            require(input_tokens >= 891 && input_tokens <= 990, "test prompt must fit above the proactive threshold");

            lighter::EventLoop loop;
            auto task = agent.run_task(prompt, {});
            loop.schedule(task);
            loop.run();
            auto outcome = task.result();
            auto valid = agent.session.validate();
            require(valid.has_value(), valid ? "" : valid.error().message());
            const auto checkpoints = std::ranges::count_if(agent.session.entries, [](const session::SessionEntry &entry) {
                return std::holds_alternative<session::ContextCheckpoint>(entry.payload);
            });
            if (history_size != 0 && prompt_size == 3'900) {
                require(outcome.has_error() && outcome.error().detail.contains("exceed the model input budget"),
                        "failed admission returned: " + (outcome.has_error() ? outcome.error().message() : "success") + " (history bytes " +
                            std::to_string(history_size) + ")");
                require(state->summaries.size() == 1 && state->requests.empty() && checkpoints == 0,
                        "failed admission committed an unusable checkpoint or called normal completion");
                require(std::get<session::TaskStarted>(agent.session.entries[5].payload).text == prompt,
                        "failed admission changed the saved user prompt");
                continue;
            }
            require(outcome.has_value() && state->requests.size() == 1, "a fitting prompt did not reach normal completion");
            require(state->summaries.size() == (history_size == 1'000 ? 1 : 0) && checkpoints == (history_size == 1'000 ? 1 : 0),
                    "admission compacted a fresh prompt or repeatedly compacted an otherwise usable request");
            const auto &request = state->requests.front();
            require(std::get<provider::TextPart>(request.back().parts[0]).text == prompt,
                    "the pending prompt was shortened or did not follow the checkpoint");
            auto replay = agent.context_manifest();
            require(replay && std::ranges::count_if(replay->provider_history,
                                                    [&prompt](const provider::Item &item) {
                                                        return item.role == provider::Role::USER && item.parts.size() == 1 &&
                                                               std::holds_alternative<provider::TextPart>(item.parts[0]) &&
                                                               std::get<provider::TextPart>(item.parts[0]).text == prompt;
                                                    }) == 1,
                    "checkpoint replay lost or duplicated the pending prompt");
        }
    }
}

void test_recovery_preserves_pending_prompt_above_proactive_threshold() {
    ToolSet tools(std::filesystem::current_path());
    const std::string prompt(3'600, 'p');
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    auto compactions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompactDispatch>()
        .calls([compactions, &prompt](provider::History &history, provider::CompactRequest request) -> lighter::Task<void, Error> {
            for (const auto &item : history) {
                for (const auto &part : item.parts) {
                    require(std::get<provider::TextPart>(part).text != prompt, "recovery summarized the pending prompt");
                }
            }
            require(request.compacted_context_target_tokens == 84, "compaction did not reserve room for the pending prompt");
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, std::string(++*compactions == 1 ? 100 : 50, 's'));
            co_return;
        })
        .times(2);
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls(
            [completions, &prompt](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                                   const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
                require(history.size() == 3 && std::get<provider::TextPart>(history.back().parts[0]).text == prompt &&
                            context::estimate_input_tokens(history) > 891,
                        "recovery must preserve a pending prompt even when it keeps context above the proactive threshold");
                if (++*completions == 1) {
                    auto error = Error::api("context_length_exceeded", "provider counts more tokens", "req_retry", false);
                    error.code = ErrorCode::CONTEXT_EXHAUSTED;
                    co_await lighter::fail(std::move(error));
                }
                emit_message(callbacks, "done", "done");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(1'000, 'u'), std::string(1'000, 'a'));
    lighter::EventLoop loop;
    auto task = agent.run_task(prompt, {});
    loop.schedule(task);
    loop.run();
    require(task.result().has_value() && agent.session.validate().has_value(), "recovery did not admit the unchanged pending prompt");
    provider_mock.verify();
}

void test_provider_rejection_of_lone_prompt_does_not_summarize_it() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            auto error = Error::api("context_length_exceeded", "provider counts more tokens", "req_lone", false);
            error.code = ErrorCode::CONTEXT_EXHAUSTED;
            co_await lighter::fail(std::move(error));
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();
    require(outcome.has_error() && outcome.error().code == ErrorCode::CONTEXT_EXHAUSTED && outcome.error().request_id == "req_lone",
            "a lone prompt rejection must retain the provider error without a doomed compaction attempt");
    provider_mock.verify();
}

void test_provider_context_rejection_is_recovered_by_compaction() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls([](provider::History &history,
                                                               provider::CompactRequest) -> lighter::Task<void, Error> {
        require(history.size() == 3 && history.back().role == provider::Role::ASSISTANT, "recovery compaction included the pending prompt");
        history.resize(provider::instruction_prefix_size(history));
        provider::append_user(history, "SUMMARY");
        co_return;
    });
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                auto error = Error::http_status(400, "invalid_request_error", "prompt is too long", "req_long");
                error.code = ErrorCode::CONTEXT_EXHAUSTED;
                co_await lighter::fail(std::move(error));
            }
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history.back().parts[0]).text == "latest",
                    "the re-issued call did not use the recovery checkpoint");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), "answer");

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", [&events](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a context rejection before any output was not recovered");
    require(agent.session.validate().has_value(), "recovery compaction produced a session that does not validate");
    require(agent.session.entries.size() == 13 && std::holds_alternative<session::ProviderCallAborted>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[8].payload),
            "recovery did not settle the rejected round before checkpointing");
    require(std::ranges::count_if(events, [](const Event &event) { return std::holds_alternative<SessionNotice>(event); }) == 1,
            "recovery compaction must be reported like any automatic compaction");
    provider_mock.verify();
}

void test_context_exhausted_stop_is_recovered_by_compaction() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            co_return;
        });
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::CONTEXT_EXHAUSTED,
                                                           .stop_detail = "model_context_window_exceeded"};
            }
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history.back().parts[0]).text == "latest",
                    "the re-issued call did not use the recovery checkpoint");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), "answer");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a context-exhausted stop before any output was not recovered");
    require(agent.session.validate().has_value(), "recovery compaction produced a session that does not validate");
    require(agent.session.entries.size() == 13 &&
                std::holds_alternative<session::ProviderCallCompleted>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[8].payload),
            "recovery did not settle the exhausted round before checkpointing");
    provider_mock.verify();
}

void test_context_rejection_after_opaque_output_is_recovered() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            co_return;
        });
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                // Encrypted reasoning arrives before the provider gives up on
                // the request; nothing of it was shown to the user.
                callbacks.on_item_completed(provider::ProviderOpaqueItem{
                    .id = {.value = "reasoning"},
                    .part = {.provider_tag = "fake", .payload = "encrypted"},
                });
                auto error = Error::http_status(400, "invalid_request_error", "prompt is too long", "req_long");
                error.code = ErrorCode::CONTEXT_EXHAUSTED;
                co_await lighter::fail(std::move(error));
            }
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history.back().parts[0]).text == "latest",
                    "the re-issued call did not use the recovery checkpoint");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), "answer");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a context rejection after only opaque output was not recovered");
    require(agent.session.validate().has_value(), "recovery after opaque output produced a session that does not validate");
    require(agent.session.entries.size() == 14 && std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ProviderCallAborted>(agent.session.entries[7].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[9].payload),
            "recovery did not settle the round with its opaque item as omitted before checkpointing");
    provider_mock.verify();
}

void test_context_rejection_after_empty_message_is_recovered() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompactDispatch>().calls(
        [](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, "SUMMARY");
            co_return;
        });
    auto completions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([completions](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                             const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if ((*completions)++ == 0) {
                emit_message(callbacks, "empty", "");
                auto error = Error::http_status(400, "invalid_request_error", "prompt is too long", "req_long");
                error.code = ErrorCode::CONTEXT_EXHAUSTED;
                co_await lighter::fail(std::move(error));
            }
            require(history.size() == 3 && std::get<provider::TextPart>(history[1].parts[0]).text == "SUMMARY" &&
                        std::get<provider::TextPart>(history.back().parts[0]).text == "latest",
                    "the re-issued call did not use the recovery checkpoint");
            emit_message(callbacks, "done", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), "answer");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();

    require(task.result().has_value(), "a context rejection after an empty message was not recovered");
    require(agent.session.validate().has_value(), "recovery after an empty message produced a session that does not validate");
    require(agent.session.entries.size() == 14 && std::holds_alternative<session::OutputItemCompleted>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ProviderCallAborted>(agent.session.entries[7].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[9].payload),
            "recovery did not settle the round with its empty message as omitted before checkpointing");
    provider_mock.verify();
}

void test_context_rejection_after_visible_output_is_not_recovered() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    provider_mock.expect<provider::CompleteDispatch>().calls(
        [](const provider::History &, const std::vector<provider::ToolDefinition> &,
           const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            emit_message(callbacks, "partial", "partial answer", /*stream*/ true);
            emit_message(callbacks, "empty", "");
            auto error = Error::http_status(400, "invalid_request_error", "prompt is too long", "req_long");
            error.code = ErrorCode::CONTEXT_EXHAUSTED;
            co_await lighter::fail(std::move(error));
        });
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 1'000, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), "answer");

    lighter::EventLoop loop;
    auto task = agent.run_task("latest", {});
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().code == ErrorCode::CONTEXT_EXHAUSTED,
            "a context rejection after visible output must surface instead of silently compacting");
    require(agent.session.entries.size() == 11 &&
                std::get<session::TaskFinished>(agent.session.entries.back().payload).outcome == session::TaskOutcome::FAILED,
            "the rejected round after visible output was not settled and failed");
    provider_mock.verify();
}

void test_failed_task_does_not_report_staged_compaction() {

    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    auto compactions = std::make_shared<usize>(0);
    provider_mock.expect<provider::CompactDispatch>()
        .calls([compactions](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, ++*compactions == 1 ? "FIRST SUMMARY" : "SUMMARY");
            co_return;
        })
        .times(2);
    // Rejected twice in a row: the first exhaustion is recovered by a second
    // compaction, the second is reported.
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([](const provider::History &, const std::vector<provider::ToolDefinition> &,
                  const provider::StreamCallbacks &) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::CONTEXT_EXHAUSTED};
        })
        .times(2);
    model::Choice choice{
        .handle = provider_mock.handle(),
        .entry = {.provider = "fake", .id = "test", .context_window = 80, .max_output_tokens = 10},
    };
    Agent agent(std::move(choice), tools, compact_test_instructions());
    append_session_message(agent.session, std::string(100, 'u'), std::string(100, 'a'));

    std::vector<Event> events;
    lighter::EventLoop loop;
    auto task = agent.run_task("latest", [&events](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_error() && outcome.error().detail.contains("context window exhausted"),
            "failed completion did not report context exhaustion");
    require(agent.session.entries.size() == 13 && std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[6].payload) &&
                std::holds_alternative<session::ContextCheckpoint>(agent.session.entries[9].payload) &&
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

std::vector<std::string> user_texts(const provider::History &history) {
    std::vector<std::string> result;
    for (const auto &item : history) {
        if (item.role != provider::Role::USER) continue;
        for (const auto &part : item.parts)
            if (const auto *text = std::get_if<provider::TextPart>(&part)) result.push_back(text->text);
    }
    return result;
}

void test_steering_at_stream_tool_and_final_boundaries() {
    lighter::EventLoop loop;
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools, {});
    lighter::Event tools_started;
    usize started = 0;
    require(tools
                .register_tool(
                    {.definition = {.name = "gated"},
                     .execution_mode = ToolExecutionMode::PARALLEL,
                     .execute = [&](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant) -> lighter::Task<ToolOutcome, Error> {
                         if (++started == 2) tools_started.set();
                         co_await tools_started.wait();
                         if (call.id == "a")
                             require(agent.steer(*agent.active_task(), "during tools").has_value(), "tool steering rejected");
                         require(agent.session.pending_steering().size() >= 1, "steering was consumed before tools settled");
                         co_return ToolOutcome{.call_id = call.id, .payload = "ok"};
                     }})
                .has_value(),
            "failed to register gated tool");
    usize completions = 0;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([&](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                   const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if (++completions == 1) {
                emit_message(callbacks, "initial", "investigating", true);
                const auto steering = agent.steer(*agent.active_task(), "during stream");
                require(steering.has_value(), steering ? "" : "stream steering rejected: " + steering.error().message());
                auto removed = agent.steer(*agent.active_task(), "withdrawn");
                require(removed && agent.session.withdraw_steering(*removed), "pending edit failed");
                auto stale = *agent.active_task();
                ++stale.task_id.value;
                require(!agent.steer(stale, "wrong task"), "stale steering accepted");
                stale = *agent.active_task();
                stale.session_id = session::generate_session_id();
                require(!agent.steer(stale, "wrong session"), "cross-session steering accepted");
                require(!agent.steer(*agent.active_task(), "") && !agent.steer(*agent.active_task(), "\xff"),
                        "invalid steering text was accepted");
                emit_tool_call(callbacks, "a", "gated");
                emit_tool_call(callbacks, "b", "gated");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            }
            if (completions == 2) {
                require(started == 2 && user_texts(history) == std::vector<std::string>{"original", "during stream", "during tools"},
                        "steering was reordered, lost, or delivered before the parallel batch");
                emit_message(callbacks, "premature-final", "done", true);
                require(agent.steer(*agent.active_task(), "at final answer").has_value(), "final-answer steering rejected");
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            }
            require(user_texts(history) == std::vector<std::string>{"original", "during stream", "during tools", "at final answer"},
                    "final-answer steering did not continue the same task");
            emit_message(callbacks, "final", "corrected answer");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(3);
    std::vector<Event> events;
    auto task = agent.run_task("original", [&](const Event &event) noexcept { events.push_back(event); });
    loop.schedule(task);
    loop.run();
    require(task.result().has_value() && agent.session.validate(), "steered task failed");
    require(!agent.active_task() && !agent.steer({.session_id = agent.session.id, .task_id = {.value = 1}}, "late"),
            "closed task still accepted steering");
    require(agent.session.pending_steering().empty() && agent.session.next_task_id == 2, "steering created an extra task");
    require(agent.session.reply_from_latest() == "corrected answer", "copyable reply came from the superseded final answer");
    require(std::ranges::count_if(events, [](const Event &e) { return std::holds_alternative<TaskCompleted>(e); }) == 1 &&
                std::ranges::count_if(events, [](const Event &e) { return std::holds_alternative<SteeringApplied>(e); }) == 3,
            "steering emitted incorrect lifecycle events");
    provider_mock.verify();
}

void test_steering_during_compaction_and_rejection() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    Agent agent(
        {.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test", .context_window = 1000, .max_output_tokens = 10}},
        tools, compact_test_instructions());
    append_session_message(agent.session, std::string(1800, 'u'), std::string(1800, 'a'));
    usize compactions = 0;
    provider_mock.expect<provider::CompactDispatch>()
        .calls([&](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
            const auto texts = user_texts(history);
            require(std::ranges::find(texts, std::string("original")) == texts.end() &&
                        std::ranges::find(texts, std::string("correction\nexact")) == texts.end(),
                    "compaction summarized unprocessed user input");
            if (++compactions == 1) {
                require(agent.steer(*agent.active_task(), "correction\nexact").has_value(), "steering during compaction rejected");
                co_await lighter::yield();
            }
            history.resize(provider::instruction_prefix_size(history));
            provider::append_user(history, std::string(compactions == 1 ? 100 : 20, 's'));
            co_return;
        })
        .times(2);
    usize completions = 0;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([&](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                   const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            const auto texts = user_texts(history);
            require(texts.size() == 3 && texts[1] == "original" && texts[2] == "correction\nexact", "admission changed pending inputs");
            if (++completions == 1) {
                auto error = Error::protocol("provider context rejection");
                error.code = ErrorCode::CONTEXT_EXHAUSTED;
                co_await lighter::fail(std::move(error));
            }
            emit_message(callbacks, "final", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    lighter::EventLoop loop;
    auto task = agent.run_task("original", {});
    loop.schedule(task);
    loop.run();
    require(task.result().has_value() && agent.session.validate(), "steering compaction recovery failed");
    require(agent.session.pending_steering().empty(), "delivered correction remained pending");
    provider_mock.verify();
}

void test_cancelled_steering_is_not_a_follow_up() {
    ToolSet tools(std::filesystem::current_path());
    lighter::mock::Mock<provider::ProviderFacade> provider_mock;
    Agent agent({.handle = provider_mock.handle(), .entry = {.provider = "fake", .id = "test"}}, tools, {});
    lighter::CancellationSource cancellation;
    lighter::Event blocked;
    std::optional<lighter::Task<void, Error, lighter::Cancellation>> cancelled_task;
    usize completions = 0;
    provider_mock.expect<provider::CompleteDispatch>()
        .calls([&](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                   const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            if (++completions == 1) {
                require(agent.steer(*agent.active_task(), "keep me").has_value(), "steering before cancellation failed");
                cancellation.cancel();
                co_await blocked.wait();
                co_return provider::ProviderCallCompletion{};
            }
            cancelled_task.reset();
            require(agent.active_task().has_value(), "destroying a cancelled task closed a newer task's input");
            const auto texts = user_texts(history);
            require(std::ranges::find(texts, std::string("keep me")) == texts.end(), "cancelled steering leaked into another task");
            emit_message(callbacks, "final", "done");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
        })
        .times(2);
    lighter::EventLoop loop;
    cancelled_task.emplace(agent.run_task("first", {}, cancellation.token()));
    loop.schedule(*cancelled_task);
    loop.run();
    require(cancelled_task->result().is_cancelled() && !agent.active_task() && agent.session.pending_steering().size() == 1,
            "cancellation lost pending steering or left admission open");
    auto next = agent.run_task("second", {});
    loop.schedule(next);
    loop.run();
    require(next.result().has_value() && agent.session.validate() && agent.session.pending_steering().size() == 1,
            "later task consumed abandoned steering");
    provider_mock.verify();
}

void test_pending_steering_changes_during_compaction() {
    for (const bool withdraw : {true, false}) {
        ToolSet tools(std::filesystem::current_path());
        lighter::mock::Mock<provider::ProviderFacade> provider_mock;
        Agent agent({.handle = provider_mock.handle(),
                     .entry = {.provider = "fake", .id = "test", .context_window = 1000, .max_output_tokens = 10}},
                    tools, {});
        session::EntryId pending;
        usize completions = 0;
        provider_mock.expect<provider::CompleteDispatch>()
            .calls([&](const provider::History &history, const std::vector<provider::ToolDefinition> &,
                       const provider::StreamCallbacks &callbacks) -> lighter::Task<provider::ProviderCallCompletion, Error> {
                if (++completions == 1) {
                    emit_message(callbacks, "initial", std::string(3600, 'a'));
                    auto accepted = agent.steer(*agent.active_task(), std::string(5000, 'u'));
                    require(accepted.has_value(), "oversized input should remain editable until admission");
                    pending = *accepted;
                } else {
                    require(user_texts(history) == std::vector<std::string>{"summary", "short"},
                            "compaction delivered stale input after withdrawal");
                    emit_message(callbacks, "final", "done");
                }
                co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE};
            })
            .times(withdraw ? 2 : 1);
        provider_mock.expect<provider::CompactDispatch>().calls(
            [&](provider::History &history, provider::CompactRequest) -> lighter::Task<void, Error> {
                require(user_texts(history) == std::vector<std::string>{"original"}, "compaction summarized pending steering");
                if (withdraw) {
                    require(agent.session.withdraw_steering(pending).has_value() && agent.steer(*agent.active_task(), "short").has_value(),
                            "pending edit during compaction failed");
                    co_await lighter::yield();
                }
                history.clear();
                provider::append_user(history, "summary");
                co_return;
            });
        lighter::EventLoop loop;
        auto task = agent.run_task("original", {});
        loop.schedule(task);
        loop.run();
        require(agent.session.validate().has_value() && !agent.active_task(), "admission left invalid task state");
        if (withdraw) {
            require(task.result().has_value() && agent.session.pending_steering().empty(), "edited input failed admission");
        } else {
            require(task.result().has_error() && agent.session.pending_steering().size() == 1 &&
                        agent.session.pending_steering()[0].id == pending,
                    "failed admission lost or delivered oversized input");
        }
        provider_mock.verify();
    }
}

i32 run_all() {
    test_steering_at_stream_tool_and_final_boundaries();
    test_steering_during_compaction_and_rejection();
    test_cancelled_steering_is_not_a_follow_up();
    test_pending_steering_changes_during_compaction();
    test_successful_task();
    test_long_tool_loop();
    test_tool_output_budget_tracks_model_context();
    test_insufficient_batch_capacity_prevents_tool_dispatch();
    test_tool_payload_framing_is_reserved_during_planning();
    test_tool_error_results_share_the_output_guard();
    test_throwing_tool_settles_its_batch();
    test_tool_waits_for_committed_output_allowance();
    test_done_requires_terminal_answer();
    test_failed_completed_round_closes_queued_tool_calls();
    test_final_phase_does_not_bypass_tool_follow_up();
    test_cancelled_task_retains_semantic_progress();
    test_stream_failure_retains_queued_tool_result();
    test_invalid_tool_is_rejected_before_dispatch();
    test_tool_execution_semantics();
    test_local_compaction_summarizes_tool_rounds_as_text();
    test_local_compaction_bounds_retained_prompts();
    test_local_compaction_drops_oldest_items_in_batches_when_the_summary_request_overflows();
    test_automatic_compaction();
    test_multiple_automatic_compactions_in_one_task();
    test_proactive_compaction_uses_reported_context();
    test_estimated_context_triggers_compaction_without_reported_usage();
    test_ineffective_automatic_compaction_fails_the_task();
    test_checkpoint_is_judged_on_its_replay();
    test_oversized_prompt_is_rejected_before_any_provider_call();
    test_automatic_compaction_through_the_local_summarizer();
    test_pending_prompt_admission_with_bounded_local_compaction();
    test_recovery_preserves_pending_prompt_above_proactive_threshold();
    test_provider_rejection_of_lone_prompt_does_not_summarize_it();
    test_provider_context_rejection_is_recovered_by_compaction();
    test_context_exhausted_stop_is_recovered_by_compaction();
    test_context_rejection_after_visible_output_is_not_recovered();
    test_context_rejection_after_opaque_output_is_recovered();
    test_context_rejection_after_empty_message_is_recovered();
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
