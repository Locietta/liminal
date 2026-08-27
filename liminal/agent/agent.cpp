#include "agent.h"

#include "default_instructions.h"
#include "tool_executor.h"
#include "tool_planner.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <string>
#include <utility>

#include <lighter/async/vocab/cancellation.h>

#include <liminal/text.h>

namespace liminal {

using lighter::cancel;
using lighter::fail;
using lighter::Task;

namespace {

constexpr std::string_view k_automatic_compact_instructions =
    "Preserve the user's goals, constraints, decisions, important tool results, modified files, unresolved issues, and the context needed "
    "to continue the active task.";
constexpr u64 k_automatic_compact_numerator = 9;
constexpr u64 k_automatic_compact_denominator = 10;
constexpr auto k_tool_cancel_grace_period = std::chrono::milliseconds(250);
constexpr usize k_provider_diagnostic_limit = 4096;
constexpr u64 k_tool_output_bytes_per_token = 4;
constexpr u64 k_tool_output_context_numerator = 1;
constexpr u64 k_tool_output_context_denominator = 2;
constexpr u64 k_tool_output_remaining_numerator = 9;
constexpr u64 k_tool_output_remaining_denominator = 10;

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

usize tool_payload_capacity(const context::ContextManifest &manifest) {
    if (!manifest.budget.context_window_tokens || !manifest.usage.remaining_input_tokens) return k_max_tool_payload_bytes;

    // Match KimiX's context-aware shape: at most half of the total context,
    // at most 90% of the currently remaining input capacity, and always below
    // Liminal's absolute local-tool ceiling.
    const auto context_bytes = static_cast<u64>(*manifest.budget.context_window_tokens) * k_tool_output_bytes_per_token *
                               k_tool_output_context_numerator / k_tool_output_context_denominator;
    const auto remaining_tokens = static_cast<u64>(std::max<i64>(*manifest.usage.remaining_input_tokens, 0));
    const auto remaining_bytes =
        remaining_tokens * k_tool_output_bytes_per_token * k_tool_output_remaining_numerator / k_tool_output_remaining_denominator;
    return static_cast<usize>(std::min<u64>({context_bytes, remaining_bytes, k_max_tool_payload_bytes}));
}

std::string bounded_diagnostic(std::string detail) { return bounded_utf8(detail, k_provider_diagnostic_limit); }

std::vector<ToolOutcome> not_started_outcomes(const std::vector<provider::ToolCall> &calls, std::string_view reason) {
    std::vector<ToolOutcome> outcomes;
    outcomes.reserve(calls.size());
    for (const auto &call : calls) {
        outcomes.push_back({.call_id = call.id, .kind = ToolOutcomeKind::NOT_STARTED, .receipt = "reason: " + std::string(reason)});
    }
    return outcomes;
}

session::ProviderCallLoopOutcome loop_outcome(const provider::ProviderCallCompletion &completion, usize call_count,
                                              bool has_terminal_answer) {
    if (completion.stop == provider::StopKind::DONE) {
        if (call_count != 0) return session::ProviderCallLoopOutcome::FOLLOW_UP;
        return has_terminal_answer ? session::ProviderCallLoopOutcome::TERMINAL : session::ProviderCallLoopOutcome::FAILED;
    }
    if (completion.stop == provider::StopKind::NEEDS_TOOL_RESULTS && call_count != 0) {
        return session::ProviderCallLoopOutcome::FOLLOW_UP;
    }
    return session::ProviderCallLoopOutcome::FAILED;
}

template <typename T, typename E>
Task<lighter::Outcome<T, E, lighter::Cancellation>> observe_cancellation(Task<T, E> work,
                                                                         const std::optional<lighter::CancellationToken> &cancellation) {
    if (cancellation) co_return co_await lighter::with_token(std::move(work), *cancellation);
    co_return co_await std::move(work).catch_cancel();
}

} // namespace

Agent::Agent(model::Choice model, ToolSet &tools) : Agent(std::move(model), tools, default_agent_instructions()) {}

Agent::Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions)
    : Agent(std::move(model), tools, std::move(instructions), session::Session{}) {}

Agent::Agent(model::Choice model, ToolSet &tools, std::vector<context::InstructionSource> instructions, session::Session session)
    : model(std::move(model)), tools(&tools), instructions(std::move(instructions)), session(std::move(session)) {
    this->session.set_model_preference(this->model.entry.provider, this->model.entry.id, this->model.reasoning_effort);
}

void Agent::select_model(model::Choice next) {
    model = std::move(next);
    session.set_model_preference(model.entry.provider, model.entry.id, model.reasoning_effort);
}

void Agent::replace_session(model::Choice next_model, session::Session next_session) {
    model = std::move(next_model);
    session = std::move(next_session);
    session.set_model_preference(model.entry.provider, model.entry.id, model.reasoning_effort);
}

Result<context::ContextManifest> Agent::context_manifest() const {
    return context::ContextBuilder{}.build(instructions, session, context_budget(model.entry));
}

Task<void, Error, lighter::Cancellation> Agent::run_task(std::string prompt, EventSink events,
                                                         std::optional<lighter::CancellationToken> cancellation) {
    const auto task_id = session.start_task(std::move(prompt));
    const auto activity_task_generation = next_activity_task_generation++;
    auto outcome = co_await run_task_loop(task_id, activity_task_generation, events, cancellation);
    if (outcome.is_cancelled()) {
        session.append(session::TaskFinished{.id = task_id, .outcome = session::TaskOutcome::CANCELLED});
        co_await cancel();
    }
    if (outcome.has_error()) {
        session.append(session::TaskFinished{.id = task_id, .outcome = session::TaskOutcome::FAILED});
        co_await fail(std::move(outcome).error());
    }
    session.append(session::TaskFinished{.id = task_id, .outcome = session::TaskOutcome::COMPLETED});
    if (outcome.has_value() && *outcome) emit(events, SessionNotice{.text = "[history compacted automatically]\n"});
    emit(events, TaskCompleted{.task_generation = activity_task_generation});
}

Task<bool, Error, lighter::Cancellation> Agent::run_task_loop(session::TaskId task_id, u64 activity_task_generation,
                                                              const EventSink &events,
                                                              const std::optional<lighter::CancellationToken> &cancellation) {
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
            auto compacted_result =
                co_await observe_cancellation(model.handle->compact(compacted, k_automatic_compact_instructions), cancellation);
            if (compacted_result.is_cancelled()) co_await cancel();
            if (compacted_result.has_error()) co_await fail(std::move(compacted_result).error());
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
        const ActivityScope activity_scope{
            .task_generation = activity_task_generation,
            .provider_call_generation = next_activity_provider_call_generation++,
        };
        std::vector<provider::ToolCall> tool_calls;
        bool has_terminal_answer = false;
        provider::StreamCallbacks stream{
            .on_assistant_text_delta =
                [&events, activity_scope](const provider::OutputItemId &item_id, std::string_view text) {
                    emit(events, AssistantTextDelta{.item_id = item_id.value, .text = std::string(text), .activity_scope = activity_scope});
                },
            .on_item_completed =
                [this, task_id, provider_call_id, activity_scope, &events, &tool_calls,
                 &has_terminal_answer](const provider::OutputItem &item) {
                    session.append(session::OutputItemCompleted{
                        .task_id = task_id,
                        .provider_call_id = provider_call_id,
                        .item = item,
                    });
                    if (const auto *message = std::get_if<provider::AssistantMessageItem>(&item)) {
                        std::string text;
                        for (const auto &part : message->parts) text += part.text;
                        emit(events, AssistantMessageCompleted{
                                         .item_id = message->id.value,
                                         .text = std::move(text),
                                         .phase = message->phase,
                                         .activity_scope = activity_scope,
                                     });
                        if (message->phase != provider::MessagePhase::COMMENTARY &&
                            std::ranges::any_of(message->parts, [](const provider::TextPart &part) { return !part.text.empty(); })) {
                            has_terminal_answer = true;
                        }
                    }
                    if (const auto *call = std::get_if<provider::ToolCallItem>(&item)) {
                        tool_calls.push_back(call->call);
                    }
                },
        };
        auto completed =
            co_await observe_cancellation(model.handle->complete(built->provider_history, tools->definitions(), stream), cancellation);
        if (completed.is_cancelled()) {
            session.append(session::ProviderCallAborted{
                .task_id = task_id,
                .id = provider_call_id,
                .reason = session::ProviderCallAbortReason::CANCELLED,
                .detail = "provider call cancelled",
            });
            auto outcomes = not_started_outcomes(tool_calls, "provider_call_cancelled");
            if (!outcomes.empty()) {
                session.append(session::ToolOutcomes{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .outcomes = std::move(outcomes),
                });
            }
            session.append(session::ProviderRoundSettled{
                .task_id = task_id,
                .provider_call_id = provider_call_id,
                .replay = session::ProviderRoundReplay::OMIT,
            });
            co_await cancel();
        }
        if (completed.has_error()) {
            const auto failure_detail = bounded_diagnostic(completed.error().message());
            session.append(session::ProviderCallAborted{
                .task_id = task_id,
                .id = provider_call_id,
                .reason = session::ProviderCallAbortReason::FAILED,
                .detail = failure_detail,
            });
            auto outcomes = not_started_outcomes(tool_calls, "provider_call_failed");
            if (!outcomes.empty()) {
                session.append(session::ToolOutcomes{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .outcomes = std::move(outcomes),
                });
            }
            session.append(session::ProviderRoundSettled{
                .task_id = task_id,
                .provider_call_id = provider_call_id,
                .replay = session::ProviderRoundReplay::OMIT,
            });
            co_await fail(std::move(completed).error());
        }
        auto completion = *std::move(completed);
        const auto call_count = tool_calls.size();
        session.metadata.working_directory = tools->working_directory.generic_string();
        session.append(session::ProviderCallCompleted{
            .task_id = task_id,
            .id = provider_call_id,
            .completion = completion,
            .loop_outcome = loop_outcome(completion, call_count, has_terminal_answer),
        });
        const auto round_outcome = loop_outcome(completion, call_count, has_terminal_answer);
        if (call_count != 0 && round_outcome != session::ProviderCallLoopOutcome::FAILED) {
            agent::detail::ToolBatchPlanner planner(*tools, [this](std::span<const ToolOutcome> projected) -> std::optional<usize> {
                auto current = context::ContextBuilder{}.build(instructions, session, context_budget(model.entry), projected);
                if (!current) return std::nullopt;
                return tool_payload_capacity(*current);
            });
            auto admission = planner.plan(std::move(tool_calls));
            if (auto *rejected = std::get_if<agent::detail::ToolBatchRejected>(&admission)) {
                session.append(session::ToolOutcomes{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .outcomes = std::move(rejected->outcomes),
                });
                session.append(session::ProviderRoundSettled{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .replay = session::ProviderRoundReplay::OMIT,
                });
                co_await fail(Error::protocol("insufficient output capacity for tool outcomes; compact first"));
            }
            agent::detail::ToolExecutor executor(*tools, std::move(*std::get_if<agent::detail::ToolBatchPlan>(&admission)), events,
                                                 activity_scope);
            auto settled = co_await observe_cancellation(executor.finish(), cancellation);
            const bool cancelled_while_settling = settled.is_cancelled();
            std::vector<ToolOutcome> outcomes;
            if (settled.is_cancelled()) {
                outcomes = co_await executor.cancel_and_finish(k_tool_cancel_grace_period);
            } else if (settled.has_value()) {
                outcomes = *std::move(settled);
            }
            if (!outcomes.empty()) {
                session.append(session::ToolOutcomes{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .outcomes = std::move(outcomes),
                });
            }
            session.append(session::ProviderRoundSettled{
                .task_id = task_id,
                .provider_call_id = provider_call_id,
                .replay = session::ProviderRoundReplay::REPLAY,
            });
            if (cancelled_while_settling) co_await cancel();
        } else {
            if (call_count != 0) {
                session.append(session::ToolOutcomes{
                    .task_id = task_id,
                    .provider_call_id = provider_call_id,
                    .outcomes = not_started_outcomes(tool_calls, "provider_round_failed"),
                });
            }
            session.append(session::ProviderRoundSettled{
                .task_id = task_id,
                .provider_call_id = provider_call_id,
                .replay = round_outcome == session::ProviderCallLoopOutcome::FAILED ? session::ProviderRoundReplay::OMIT :
                                                                                      session::ProviderRoundReplay::REPLAY,
            });
        }

        switch (completion.stop) {
            case provider::StopKind::DONE:
                if (call_count == 0) {
                    if (!has_terminal_answer) {
                        co_await fail(Error::protocol("provider completed the task without a terminal assistant answer"));
                    }
                    emit(events, ProviderActivityCompleted{.activity_scope = activity_scope});
                    co_return automatically_compacted;
                }
                emit(events, ProviderActivityCompleted{.activity_scope = activity_scope});
                break;
            case provider::StopKind::NEEDS_TOOL_RESULTS: emit(events, ProviderActivityCompleted{.activity_scope = activity_scope}); break;
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
