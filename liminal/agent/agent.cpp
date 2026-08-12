#include "agent.h"

#include "default_instructions.h"
#include "tool_scheduler.h"

#include <string>
#include <utility>

namespace liminal {

using lighter::fail;
using lighter::Task;

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

} // namespace

Agent::Agent(model::Choice model, ToolSet &tools) : Agent(std::move(model), tools, default_agent_instructions()) {}

Agent::Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions)
    : model(std::move(model)), tools(&tools), instructions(std::move(instructions)) {}

Result<context::ContextManifest> Agent::context_manifest() const {
    return context::ContextBuilder{}.build(instructions, session, context_budget(model.entry));
}

Task<void, Error> Agent::run_turn(std::string prompt, EventSink events) {
    const auto task_id = session.start_task(std::move(prompt));
    auto outcome = co_await run_task(task_id, events);
    if (!outcome) {
        session.append(session::TaskFinished{.id = task_id, .outcome = session::TaskOutcome::FAILED});
        co_await fail(std::move(outcome).error());
    }
    session.append(session::TaskFinished{.id = task_id, .outcome = session::TaskOutcome::COMPLETED});
    if (*outcome) emit(events, SessionNotice{.text = "[history compacted automatically]\n"});
    emit(events, TurnCompleted{});
}

Task<bool, Error> Agent::run_task(session::TaskId task_id, const EventSink &events) {
    bool automatically_compacted = false;
    while (true) {
        context::ContextBuilder builder;
        auto built = builder.build(instructions, session, context_budget(model.entry));
        if (!built) {
            co_await fail(std::move(built).error());
        }
        if (needs_automatic_compaction(*built)) {
            auto full = builder.build(instructions, session);
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
            session.append(*std::move(checkpoint));
            automatically_compacted = true;

            built = builder.build(instructions, session, context_budget(model.entry));
            if (!built) {
                co_await fail(std::move(built).error());
            }
            if (needs_automatic_compaction(*built)) {
                co_await fail(Error::protocol("automatic compaction did not reduce context below its threshold"));
            }
        }
        const auto provider_call_id = session.next_provider_call();
        usize call_count = 0;
        agent::detail::ToolScheduler scheduler(*tools, events);
        provider::StreamCallbacks stream{
            .on_assistant_text_delta = [&events](const provider::OutputItemId &,
                                                 std::string_view text) { emit(events, AssistantTextDelta{.text = std::string(text)}); },
            .on_item_completed =
                [this, task_id, provider_call_id, &scheduler, &call_count](const provider::OutputItem &item) {
                    session.append(session::OutputItemCompleted{
                        .task_id = task_id,
                        .provider_call_id = provider_call_id,
                        .item = item,
                    });
                    if (const auto *call = std::get_if<provider::ToolCallItem>(&item)) {
                        ++call_count;
                        scheduler.submit(call->call);
                    }
                },
        };
        auto completed = co_await model.handle->complete(built->provider_history, tools->definitions(), stream);
        if (!completed) {
            auto results = co_await scheduler.finish();
            if (!results.empty()) {
                session.append(session::ToolResults{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .results = std::move(results),
                });
            }
            co_await fail(std::move(completed).error());
        }
        auto completion = *std::move(completed);
        session.append(session::ProviderCallCompleted{
            .task_id = task_id,
            .id = provider_call_id,
            .completion = completion,
        });
        emit(events, AssistantSegmentCompleted{});
        auto results = co_await scheduler.finish();
        if (!results.empty()) {
            session.append(session::ToolResults{
                .task_id = task_id,
                .provider_call_id = provider_call_id,
                .results = std::move(results),
            });
        }

        switch (completion.stop) {
            case provider::StopKind::DONE:
                if (call_count == 0) co_return automatically_compacted;
                break;
            case provider::StopKind::NEEDS_TOOL_RESULTS: break;
            case provider::StopKind::TRUNCATED:
                co_await fail(Error::protocol("response truncated (" + completion.stop_detail + "); raise max_tokens"));
            case provider::StopKind::CONTEXT_EXHAUSTED: co_await fail(Error::protocol("context window exhausted; try /compact"));
            case provider::StopKind::REFUSED: co_await fail(Error::protocol("the model refused to continue this conversation"));
            case provider::StopKind::OTHER: co_await fail(Error::protocol("unsupported stop reason: " + completion.stop_detail));
        }
        if (call_count == 0) {
            co_await fail(Error::protocol("provider requested tool results without any tool calls"));
        }
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
        session.append(*std::move(checkpoint));
    }
}

} // namespace liminal
