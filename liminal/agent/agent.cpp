#include "agent.h"

#include <string>
#include <utility>
#include <vector>

#include <lighter/async/runtime/when.h>
#include <lighter/async/runtime/sync.h>

namespace liminal {

using lighter::fail;
using lighter::Task;
using lighter::WhenAll;

namespace {

constexpr std::string_view k_automatic_compact_instructions =
    "Preserve the user's goals, constraints, decisions, important tool results, modified files, unresolved issues, and the context needed "
    "to continue the active task.";

void emit(const EventSink &events, Event event) {
    if (events) {
        events(event);
    }
}

session::AgentOutput agent_output(provider::TurnResponse response) {
    return {
        .parts = std::move(response.parts),
        .usage = response.usage,
        .model = std::move(response.model),
        .request_id = std::move(response.request_id),
    };
}

context::ContextBudget context_budget(const model::Entry &model) {
    return {
        .context_window_tokens = model.context_window,
        .reserved_output_tokens = model.max_output_tokens,
        .safety_margin_tokens = model.context_safety_margin_tokens,
    };
}

/// Runs one tool call, converting infrastructure failures into is_error
/// results so one bad call never aborts its siblings. Cancellation still
/// unwinds normally.
struct ToolPermit {
    lighter::Semaphore *slots;

    ~ToolPermit() { slots->release(); }
};

Task<provider::ToolResult> execute_one(const ToolSet &tools, const provider::ToolCall &call, const EventSink &events,
                                       lighter::Semaphore &slots) {
    co_await slots.acquire();
    ToolPermit permit{&slots};
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
    emit(events, ToolCompleted{.call_id = call.id, .name = call.name, .is_error = result.is_error});
    co_return result;
}

} // namespace

Agent::Agent(model::Choice model, ToolSet &tools)
    : Agent(std::move(model), tools,
            {{
                .authority = context::InstructionAuthority::APPLICATION,
                .trust = context::InstructionTrust::PLATFORM,
                .origin = "builtin:default-agent",
                .content = "You are a helpful coding assistant.",
            }}) {}

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

    provider::StreamCallbacks stream{
        .on_text_delta = [&events](std::string_view text) { emit(events, AssistantTextDelta{.text = std::string(text)}); },
    };

    constexpr i32 k_max_iterations = 32;
    usize tool_calls_used = 0;
    bool automatically_compacted = false;
    lighter::Semaphore tool_slots(static_cast<isize>(tools->policy.max_parallel_calls));
    for (i32 iteration = 0; iteration < k_max_iterations; ++iteration) {
        context::ContextBuilder builder;
        auto built = builder.build(instructions, staged, context_budget(model.entry));
        if (!built) {
            co_await fail(std::move(built).error());
        }
        if (built->omitted_budget_entries != 0) {
            if (automatically_compacted) {
                co_await fail(Error::protocol("context exceeded the model input budget again after automatic compaction"));
            }
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
            if (built->omitted_budget_entries != 0) {
                co_await fail(Error::protocol("automatic compaction did not reduce context below the model input budget"));
            }
        }
        auto response = co_await model.handle->complete(built->provider_history, tools->definitions(), stream).or_fail();
        auto calls = provider::tool_calls(response);
        emit(events, AssistantSegmentCompleted{});

        switch (response.stop) {
            case provider::StopKind::DONE:
                staged.append(agent_output(std::move(response)));
                session = std::move(staged);
                if (automatically_compacted) {
                    emit(events, SessionNotice{.text = "[history compacted automatically]\n"});
                }
                emit(events, TurnCompleted{});
                co_return;
            case provider::StopKind::NEEDS_TOOL_RESULTS: break;
            case provider::StopKind::TRUNCATED:
                co_await fail(Error::protocol("response truncated (" + response.stop_detail + "); raise max_tokens"));
            case provider::StopKind::CONTEXT_EXHAUSTED: co_await fail(Error::protocol("context window exhausted; try /compact"));
            case provider::StopKind::REFUSED: co_await fail(Error::protocol("the model refused to continue this conversation"));
            case provider::StopKind::OTHER: co_await fail(Error::protocol("unsupported stop reason: " + response.stop_detail));
        }
        if (calls.empty()) {
            co_await fail(Error::protocol("provider requested tool results without any tool calls"));
        }
        if (calls.size() > tools->policy.max_calls_per_turn - tool_calls_used) {
            co_await fail(
                Error::tool("tool call budget exceeded (maximum " + std::to_string(tools->policy.max_calls_per_turn) + " per turn)"));
        }
        tool_calls_used += calls.size();

        std::vector<Task<provider::ToolResult>> pending;
        pending.reserve(calls.size());
        for (const auto *call : calls) {
            emit(events, ToolStarted{.call_id = call->id, .name = call->name});
            pending.push_back(execute_one(*tools, *call, events, tool_slots));
        }
        auto joined = co_await WhenAll(std::move(pending));

        staged.append(agent_output(std::move(response)));
        std::vector<provider::ToolResult> results;
        results.reserve(joined.size());
        for (auto &result : joined) {
            results.push_back(std::move(result));
        }
        staged.append(session::ToolResults{.results = std::move(results)});
    }
    co_await fail(Error::protocol("exceeded " + std::to_string(k_max_iterations) + " tool iterations in one turn"));
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
