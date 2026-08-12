#include "agent.h"

#include "default_instructions.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <lighter/async/runtime/when.h>

namespace liminal {

using lighter::fail;
using lighter::Task;
using lighter::WhenAll;

namespace {

constexpr std::string_view k_automatic_compact_instructions =
    "Preserve the user's goals, constraints, decisions, important tool results, modified files, unresolved issues, and the context needed "
    "to continue the active task.";
constexpr u64 k_automatic_compact_numerator = 9;
constexpr u64 k_automatic_compact_denominator = 10;

void emit(const EventSink &events, Event event) {
    if (events) {
        events(event);
    }
}

session::AgentOutput agent_output(const std::vector<provider::OutputItem> &items, provider::ProviderCallCompletion completion) {
    session::AgentOutput output{
        .usage = completion.usage,
        .model = std::move(completion.model),
        .request_id = std::move(completion.request_id),
    };
    for (const auto &item : items) {
        std::visit(
            [&output](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, provider::AssistantMessageItem>) {
                    for (const auto &part : value.parts) output.parts.push_back(part);
                } else if constexpr (std::is_same_v<T, provider::ToolCallItem>) {
                    output.parts.push_back(value.call);
                } else {
                    output.parts.push_back(value.part);
                }
            },
            item);
    }
    return output;
}

context::ContextBudget context_budget(const model::Entry &model) {
    return {
        .context_window_tokens = model.context_window,
        .reserved_output_tokens = model.max_output_tokens,
        .safety_margin_tokens = model.context_safety_margin_tokens,
    };
}

bool needs_automatic_compaction(const context::ContextManifest &manifest) {
    if (manifest.omitted_budget_entries != 0) {
        return true;
    }
    if (!manifest.budget.context_window_tokens || !manifest.usage.reported_context_tokens) {
        return false;
    }
    const auto threshold =
        static_cast<u64>(*manifest.budget.context_window_tokens) * k_automatic_compact_numerator / k_automatic_compact_denominator;
    return manifest.usage.estimated_input_tokens >= threshold;
}

/// Runs one tool call, converting infrastructure failures into is_error
/// results so one bad call never aborts its siblings. Cancellation still
/// unwinds normally.
Task<provider::ToolResult> execute_one(const ToolSet &tools, const provider::ToolCall &call, const EventSink &events) {
    auto outcome = co_await tools.execute(call);
    provider::ToolResult result;
    if (!outcome) {
        result = {
            .call_id = call.id,
            .content = "Error: " + outcome.error().message(),
            .is_error = true,
        };
    } else {
        result = *std::move(outcome);
    }
    const auto presentation = tools.describe(call);
    emit(events, ToolCompleted{
                     .call_id = call.id,
                     .name = call.name,
                     .description = presentation.description,
                     .command = presentation.command,
                     .summary = tools.summarize(call, result),
                     .is_error = result.is_error,
                 });
    co_return result;
}

void emit_tool_started(const ToolSet &tools, const provider::ToolCall &call, const EventSink &events) {
    const auto presentation = tools.describe(call);
    emit(events, ToolStarted{
                     .call_id = call.id,
                     .name = call.name,
                     .description = presentation.description,
                     .command = presentation.command,
                 });
}

/// Preserve model order while allowing adjacent, explicitly parallel-safe
/// calls to overlap. Exclusive calls are barriers before and after themselves.
Task<std::vector<provider::ToolResult>> execute_tools(const ToolSet &tools, const std::vector<const provider::ToolCall *> &calls,
                                                      const EventSink &events) {
    std::vector<provider::ToolResult> results;
    results.reserve(calls.size());

    usize next = 0;
    while (next < calls.size()) {
        if (tools.execution_mode(calls[next]->name) == ToolExecutionMode::EXCLUSIVE) {
            emit_tool_started(tools, *calls[next], events);
            results.push_back(co_await execute_one(tools, *calls[next], events));
            ++next;
            continue;
        }

        std::vector<Task<provider::ToolResult>> pending;
        while (next < calls.size() && tools.execution_mode(calls[next]->name) == ToolExecutionMode::PARALLEL) {
            emit_tool_started(tools, *calls[next], events);
            pending.push_back(execute_one(tools, *calls[next], events));
            ++next;
        }
        auto joined = co_await WhenAll(std::move(pending));
        for (auto &result : joined) results.push_back(std::move(result));
    }
    co_return results;
}

} // namespace

Agent::Agent(model::Choice model, ToolSet &tools) : Agent(std::move(model), tools, default_agent_instructions()) {}

Agent::Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions)
    : model(std::move(model)), tools(&tools), instructions(std::move(instructions)) {}

Result<context::ContextManifest> Agent::context_manifest() const {
    return context::ContextBuilder{}.build(instructions, session, context_budget(model.entry));
}

Task<void, Error> Agent::run_turn(std::string prompt, EventSink events) {
    // Transactional: staged session entries replace committed state only
    // after a complete terminal response. The UI transcript intentionally
    // remains.
    auto staged = session;
    staged.append(session::UserMessage{.text = std::move(prompt)});

    bool automatically_compacted = false;
    while (true) {
        context::ContextBuilder builder;
        auto built = builder.build(instructions, staged, context_budget(model.entry));
        if (!built) {
            co_await fail(std::move(built).error());
        }
        if (needs_automatic_compaction(*built)) {
            auto full = builder.build(instructions, staged);
            if (!full) {
                co_await fail(std::move(full).error());
            }
            auto full_manifest = *std::move(full);
            auto compacted = std::move(full_manifest.provider_history);
            co_await model.handle->compact(compacted, k_automatic_compact_instructions).or_fail();
            auto checkpoint = builder.take_checkpoint(std::move(compacted), full_manifest);
            if (!checkpoint) {
                co_await fail(std::move(checkpoint).error());
            }
            if (checkpoint->items.empty()) {
                co_await fail(Error::protocol("automatic compaction produced an empty checkpoint"));
            }
            staged.append(*std::move(checkpoint));
            automatically_compacted = true;

            built = builder.build(instructions, staged, context_budget(model.entry));
            if (!built) {
                co_await fail(std::move(built).error());
            }
            if (needs_automatic_compaction(*built)) {
                co_await fail(Error::protocol("automatic compaction did not reduce context below its threshold"));
            }
        }
        std::vector<provider::OutputItem> items;
        provider::StreamCallbacks stream{
            .on_assistant_text_delta = [&events](const provider::OutputItemId &,
                                                 std::string_view text) { emit(events, AssistantTextDelta{.text = std::string(text)}); },
            .on_item_completed = [&items](const provider::OutputItem &item) { items.push_back(item); },
        };
        auto completion = co_await model.handle->complete(built->provider_history, tools->definitions(), stream).or_fail();
        std::vector<const provider::ToolCall *> calls;
        for (const auto &item : items) {
            if (const auto *call = std::get_if<provider::ToolCallItem>(&item)) calls.push_back(&call->call);
        }
        emit(events, AssistantSegmentCompleted{});

        switch (completion.stop) {
            case provider::StopKind::DONE:
                staged.append(agent_output(items, std::move(completion)));
                session = std::move(staged);
                if (automatically_compacted) {
                    emit(events, SessionNotice{.text = "[history compacted automatically]\n"});
                }
                emit(events, TurnCompleted{});
                co_return;
            case provider::StopKind::NEEDS_TOOL_RESULTS: break;
            case provider::StopKind::TRUNCATED:
                co_await fail(Error::protocol("response truncated (" + completion.stop_detail + "); raise max_tokens"));
            case provider::StopKind::CONTEXT_EXHAUSTED: co_await fail(Error::protocol("context window exhausted; try /compact"));
            case provider::StopKind::REFUSED: co_await fail(Error::protocol("the model refused to continue this conversation"));
            case provider::StopKind::OTHER: co_await fail(Error::protocol("unsupported stop reason: " + completion.stop_detail));
        }
        if (calls.empty()) {
            co_await fail(Error::protocol("provider requested tool results without any tool calls"));
        }

        staged.append(agent_output(items, std::move(completion)));
        auto results = co_await execute_tools(*tools, calls, events);
        staged.append(session::ToolResults{.results = std::move(results)});
    }
}

Task<void, Error> Agent::compact(std::string_view instructions) {
    auto built = context::ContextBuilder{}.build(this->instructions, session);
    if (!built) {
        co_await fail(std::move(built).error());
    }
    auto manifest = *std::move(built);
    auto compacted = std::move(manifest.provider_history);
    co_await model.handle->compact(compacted, instructions).or_fail();
    auto checkpoint = context::ContextBuilder{}.take_checkpoint(std::move(compacted), manifest);
    if (!checkpoint) {
        co_await fail(std::move(checkpoint).error());
    }
    if (!checkpoint->items.empty()) {
        auto staged = session;
        staged.append(*std::move(checkpoint));
        session = std::move(staged);
    }
}

} // namespace liminal
