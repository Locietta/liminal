#include "context.h"

#include <algorithm>
#include <concepts>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <lighter/async/vocab/outcome.h>
#include <lighter/codec/json/json.h>

namespace liminal::context {

namespace {

int authority_rank(InstructionAuthority authority) {
    switch (authority) {
        case InstructionAuthority::RUNTIME: return 0;
        case InstructionAuthority::APPLICATION: return 1;
        case InstructionAuthority::PROJECT: return 2;
    }
    std::unreachable();
}

void append_instruction(provider::History &history, const InstructionSource &source) {
    if (source.authority == InstructionAuthority::RUNTIME) {
        provider::append_system(history, source.content);
    } else {
        provider::append_developer(history, source.content);
    }
}

void append_checkpoint_item(provider::History &history, const session::CheckpointItem &item) {
    std::visit(
        [&history](const auto &value) {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<T, session::ContextInput>) {
                history.push_back({.role = provider::Role::USER, .parts = value.parts});
            } else if constexpr (std::same_as<T, session::CheckpointOutput>) {
                provider::append_output_item(history, value.item);
            }
        },
        item);
}

struct UsageBaseline {
    usize history_size;
    usize context_tokens;
};

usize to_usize(u64 value) { return static_cast<usize>(std::min<u64>(value, std::numeric_limits<usize>::max())); }

struct PendingRound {
    std::optional<session::ProviderCallId> id;
    std::vector<provider::OutputItem> output;
    std::optional<session::ProviderCallCompleted> completed;
    std::optional<session::ProviderCallAborted> aborted;
    std::optional<session::ToolOutcomes> outcomes;
};

Result<void> check_round_id(PendingRound &round, session::ProviderCallId id) {
    if (!round.id) {
        round.id = id;
    } else if (*round.id != id) {
        return lighter::outcome_error(Error::protocol("provider rounds overlap in session history"));
    }
    return {};
}

Result<void> replay_round(provider::History &history, PendingRound &round, std::span<const ToolOutcome> projected,
                          std::optional<UsageBaseline> *baseline) {
    if (!round.completed || round.aborted) {
        return lighter::outcome_error(Error::protocol("replayed provider round did not complete successfully"));
    }
    std::vector<std::string_view> calls;
    for (const auto &item : round.output) {
        if (const auto *call = std::get_if<provider::ToolCallItem>(&item)) calls.push_back(call->call.id);
    }
    const auto outcomes = round.outcomes ? std::span<const ToolOutcome>(round.outcomes->outcomes) : projected;
    if (calls.size() != outcomes.size()) {
        return lighter::outcome_error(Error::protocol("replayed provider round does not have exactly one outcome per tool call"));
    }
    for (usize index = 0; index < calls.size(); ++index) {
        if (calls[index] != outcomes[index].call_id) {
            return lighter::outcome_error(Error::protocol("replayed provider round tool outcomes are missing, duplicated, or reordered"));
        }
    }
    for (const auto &item : round.output) provider::append_output_item(history, item);
    if (baseline && round.completed->completion.usage.context_tokens != 0) {
        *baseline =
            UsageBaseline{.history_size = history.size(), .context_tokens = to_usize(round.completed->completion.usage.context_tokens)};
    }
    if (!outcomes.empty()) provider::append_tool_outcomes(history, std::vector<ToolOutcome>(outcomes.begin(), outcomes.end()));
    return {};
}

Result<void> project_entries(provider::History &history, std::span<const session::SessionEntry *const> entries,
                             std::span<const ToolOutcome> projected, std::optional<UsageBaseline> *baseline = nullptr) {
    PendingRound round;
    for (const auto *entry : entries) {
        if (const auto *started = std::get_if<session::TaskStarted>(&entry->payload)) {
            if (round.id) return lighter::outcome_error(Error::protocol("task boundary splits an unsettled provider round"));
            provider::append_user(history, started->text);
        } else if (const auto *output = std::get_if<session::OutputItemCompleted>(&entry->payload)) {
            if (auto valid = check_round_id(round, output->provider_call_id); !valid) return valid;
            round.output.push_back(output->item);
        } else if (const auto *completed = std::get_if<session::ProviderCallCompleted>(&entry->payload)) {
            if (auto valid = check_round_id(round, completed->id); !valid) return valid;
            if (round.completed || round.aborted) {
                return lighter::outcome_error(Error::protocol("provider round has multiple remote terminal entries"));
            }
            round.completed = *completed;
        } else if (const auto *aborted = std::get_if<session::ProviderCallAborted>(&entry->payload)) {
            if (auto valid = check_round_id(round, aborted->id); !valid) return valid;
            if (round.completed || round.aborted) {
                return lighter::outcome_error(Error::protocol("provider round has multiple remote terminal entries"));
            }
            round.aborted = *aborted;
        } else if (const auto *outcomes = std::get_if<session::ToolOutcomes>(&entry->payload)) {
            if (auto valid = check_round_id(round, outcomes->provider_call_id); !valid) return valid;
            if (round.outcomes) return lighter::outcome_error(Error::protocol("provider round has multiple tool outcome entries"));
            round.outcomes = *outcomes;
        } else if (const auto *settled = std::get_if<session::ProviderRoundSettled>(&entry->payload)) {
            if (auto valid = check_round_id(round, settled->provider_call_id); !valid) return valid;
            if (settled->replay == session::ProviderRoundReplay::REPLAY) {
                if (auto replayed = replay_round(history, round, {}, baseline); !replayed) return replayed;
            }
            round = {};
        } else if (const auto *checkpoint = std::get_if<session::ContextCheckpoint>(&entry->payload)) {
            if (round.id) return lighter::outcome_error(Error::protocol("checkpoint splits an unsettled provider round"));
            for (const auto &item : checkpoint->items) append_checkpoint_item(history, item);
        } else if (std::holds_alternative<session::TaskFinished>(entry->payload) && round.id) {
            return lighter::outcome_error(Error::protocol("task finished with an unsettled provider round"));
        }
    }
    if (!round.id) {
        if (!projected.empty()) return lighter::outcome_error(Error::protocol("projected tool outcomes have no provisional round"));
        return {};
    }
    if (projected.empty()) return lighter::outcome_error(Error::protocol("session history ends with an unsettled provider round"));
    return replay_round(history, round, projected, baseline);
}

bool starts_semantic_task(const session::SessionEntry &entry) {
    return std::holds_alternative<session::TaskStarted>(entry.payload) || std::holds_alternative<session::ContextCheckpoint>(entry.payload);
}

bool matches_instruction(const provider::Item &item, const InstructionSource &source) {
    const auto expected_role = source.authority == InstructionAuthority::RUNTIME ? provider::Role::SYSTEM : provider::Role::DEVELOPER;
    if (item.role != expected_role || item.parts.size() != 1) {
        return false;
    }
    const auto *text = std::get_if<provider::TextPart>(&item.parts.front());
    return text && text->text == source.content;
}

usize part_bytes(const provider::Part &part) {
    return std::visit(
        [](const auto &value) -> usize {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<T, provider::TextPart>) {
                return value.text.size();
            } else if constexpr (std::same_as<T, provider::ToolCall>) {
                auto input = lighter::codec::json::to_string(value.input);
                return value.id.size() + value.name.size() + (input ? input->size() : 0);
            } else if constexpr (std::same_as<T, ToolOutcome>) {
                return value.call_id.size() + render_tool_outcome(value).size();
            } else if constexpr (std::same_as<T, provider::OpaquePart>) {
                return value.provider_tag.size() + value.payload.size();
            }
        },
        part);
}

usize estimate_tokens(std::span<const provider::Item> history) {
    usize bytes = 0;
    usize part_count = 0;
    for (const auto &item : history) {
        for (const auto &part : item.parts) {
            bytes += part_bytes(part);
            ++part_count;
        }
    }
    return (bytes + 3) / 4 + history.size() * 4 + part_count * 2;
}

ContextUsage estimate_usage(const provider::History &history, const ContextBudget &budget,
                            std::optional<UsageBaseline> baseline = std::nullopt) {
    ContextUsage usage;
    const auto instruction_count = provider::instruction_prefix_size(history);
    for (usize item_index = 0; item_index < history.size(); ++item_index) {
        for (const auto &part : history[item_index].parts) {
            const auto bytes = part_bytes(part);
            if (item_index < instruction_count) {
                usage.instruction_bytes += bytes;
            } else if (std::holds_alternative<provider::ToolCall>(part) || std::holds_alternative<ToolOutcome>(part)) {
                usage.tool_bytes += bytes;
            } else if (std::holds_alternative<provider::OpaquePart>(part)) {
                usage.opaque_bytes += bytes;
            } else {
                usage.conversation_bytes += bytes;
            }
        }
    }
    if (baseline && baseline->history_size <= history.size()) {
        usage.reported_context_tokens = baseline->context_tokens;
        usage.estimated_trailing_tokens = estimate_tokens(std::span(history).subspan(baseline->history_size));
        usage.estimated_input_tokens = baseline->context_tokens + usage.estimated_trailing_tokens;
    } else {
        usage.estimated_input_tokens = estimate_tokens(history);
    }
    if (budget.context_window_tokens) {
        const auto unavailable = static_cast<u64>(budget.reserved_output_tokens) + budget.safety_margin_tokens;
        usage.input_budget_tokens = unavailable < *budget.context_window_tokens ? *budget.context_window_tokens - unavailable : 0;
        usage.remaining_input_tokens = static_cast<i64>(*usage.input_budget_tokens) - static_cast<i64>(usage.estimated_input_tokens);
    }
    return usage;
}

std::string_view authority_name(InstructionAuthority authority) {
    switch (authority) {
        case InstructionAuthority::RUNTIME: return "runtime";
        case InstructionAuthority::APPLICATION: return "application";
        case InstructionAuthority::PROJECT: return "project";
    }
    std::unreachable();
}

std::string_view trust_name(InstructionTrust trust) {
    switch (trust) {
        case InstructionTrust::PLATFORM: return "platform";
        case InstructionTrust::OPERATOR: return "operator";
        case InstructionTrust::WORKSPACE: return "workspace";
    }
    std::unreachable();
}

void append_entry_ids(std::string &description, std::span<const session::EntryId> entries) {
    constexpr usize k_display_limit = 16;
    description += " [";
    const auto displayed = std::min(entries.size(), k_display_limit);
    for (usize index = 0; index < displayed; ++index) {
        if (index != 0) description += ", ";
        description += std::to_string(entries[index].value);
    }
    if (entries.size() > displayed) {
        if (displayed != 0) description += ", ";
        description += "... +" + std::to_string(entries.size() - displayed);
    }
    description += "]";
}

} // namespace

Result<ContextManifest> ContextBuilder::build(std::span<const InstructionSource> sources, const session::Session &session,
                                              ContextBudget budget, std::span<const ToolOutcome> projected_tool_outcomes) const {
    std::vector<InstructionSource> ordered(sources.begin(), sources.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto &left, const auto &right) { return authority_rank(left.authority) < authority_rank(right.authority); });

    ContextManifest manifest{.session_id = session.id, .budget = budget};
    manifest.instructions.reserve(ordered.size());
    manifest.omitted_duplicates.reserve(ordered.size());
    for (auto &source : ordered) {
        const bool duplicate =
            std::ranges::any_of(manifest.instructions, [&](const auto &resolved) { return resolved.content == source.content; });
        if (duplicate) {
            manifest.omitted_duplicates.push_back(std::move(source));
        } else {
            manifest.instructions.push_back(std::move(source));
        }
    }

    auto branch = session.active_branch();
    usize start = 0;
    for (usize index = 0; index < branch.size(); ++index) {
        if (std::holds_alternative<session::ContextCheckpoint>(branch[index]->payload)) {
            start = index;
            manifest.active_checkpoint = branch[index]->id;
        }
    }
    manifest.omitted_checkpoint_entries = start;

    provider::History instruction_history;
    instruction_history.reserve(manifest.instructions.size());
    for (const auto &source : manifest.instructions) {
        append_instruction(instruction_history, source);
    }

    usize selected_start = start;
    if (budget.context_window_tokens) {
        const auto instruction_usage = estimate_usage(instruction_history, budget);
        if (instruction_usage.remaining_input_tokens && *instruction_usage.remaining_input_tokens < 0) {
            return lighter::outcome_error(Error::config("resolved instructions exceed the model input budget"));
        }

        std::vector<usize> unit_starts;
        for (usize index = start; index < branch.size(); ++index) {
            if (index == start || starts_semantic_task(*branch[index])) {
                unit_starts.push_back(index);
            }
        }
        for (auto unit = unit_starts.rbegin(); unit != unit_starts.rend(); ++unit) {
            auto candidate = instruction_history;
            std::optional<UsageBaseline> baseline;
            auto replayed =
                project_entries(candidate, std::span(branch).subspan(*unit), projected_tool_outcomes, *unit == start ? &baseline : nullptr);
            if (!replayed) return lighter::outcome_error(std::move(replayed).error());
            const auto candidate_usage = estimate_usage(candidate, budget, baseline);
            if (*candidate_usage.remaining_input_tokens < 0) {
                if (unit == unit_starts.rbegin()) {
                    return lighter::outcome_error(Error::protocol("the latest semantic task exceeds the model input budget"));
                }
                break;
            }
            selected_start = *unit;
        }
    }

    manifest.omitted_budget_entries = selected_start - start;
    manifest.omitted_session_entries = manifest.omitted_checkpoint_entries + manifest.omitted_budget_entries;
    manifest.session_entries.reserve(branch.size() - selected_start);
    manifest.provider_history = std::move(instruction_history);
    manifest.provider_history.reserve(manifest.provider_history.size() + branch.size() - selected_start);
    std::optional<UsageBaseline> baseline;
    for (usize index = selected_start; index < branch.size(); ++index) manifest.session_entries.push_back(branch[index]->id);
    auto replayed = project_entries(manifest.provider_history, std::span(branch).subspan(selected_start), projected_tool_outcomes,
                                    selected_start == start ? &baseline : nullptr);
    if (!replayed) return lighter::outcome_error(std::move(replayed).error());
    manifest.usage = estimate_usage(manifest.provider_history, manifest.budget, baseline);
    return manifest;
}

std::string describe(const ContextManifest &manifest) {
    std::string description = "context\n";
    description += "session: " + session::to_string(manifest.session_id) + "\n";
    description += "instructions: " + std::to_string(manifest.instructions.size()) + "\n";
    for (const auto &source : manifest.instructions) {
        description +=
            "- " + std::string(authority_name(source.authority)) + " / " + std::string(trust_name(source.trust)) + " / " + source.origin;
        if (source.scope) {
            description += " / scope " + source.scope->generic_string();
        } else {
            description += " / global";
        }
        description += " / " + std::to_string(source.content.size()) + " bytes\n";
    }
    if (!manifest.omitted_duplicates.empty()) {
        description += "duplicate instructions omitted: " + std::to_string(manifest.omitted_duplicates.size()) + "\n";
        for (const auto &source : manifest.omitted_duplicates) {
            description += "- " + source.origin + "\n";
        }
    }
    description += "session entries selected: " + std::to_string(manifest.session_entries.size());
    append_entry_ids(description, manifest.session_entries);
    description += "\nsession entries omitted: " + std::to_string(manifest.omitted_session_entries) + " (checkpoint " +
                   std::to_string(manifest.omitted_checkpoint_entries) + ", budget " + std::to_string(manifest.omitted_budget_entries) +
                   ")\n";
    description += manifest.active_checkpoint ? "active checkpoint: " + std::to_string(manifest.active_checkpoint->value) + "\n" :
                                                "active checkpoint: none\n";
    description += "estimated input: " + std::to_string(manifest.usage.estimated_input_tokens) + " tokens\n";
    if (manifest.usage.reported_context_tokens) {
        description += "reported context baseline: " + std::to_string(*manifest.usage.reported_context_tokens) + " tokens\n";
        description += "estimated trailing context: " + std::to_string(manifest.usage.estimated_trailing_tokens) + " tokens\n";
    }
    if (manifest.budget.context_window_tokens) {
        description += "context window: " + std::to_string(*manifest.budget.context_window_tokens) + " tokens\n";
        description += "reserved output: " + std::to_string(manifest.budget.reserved_output_tokens) + " tokens\n";
        description += "safety margin: " + std::to_string(manifest.budget.safety_margin_tokens) + " tokens\n";
        description += "input budget: " + std::to_string(*manifest.usage.input_budget_tokens) + " tokens\n";
        description += "input remaining: " + std::to_string(*manifest.usage.remaining_input_tokens) + " tokens\n";
    } else {
        description += "context window: unknown (automatic budgeting disabled)\n";
    }
    description += "payload bytes: instructions " + std::to_string(manifest.usage.instruction_bytes) + ", conversation " +
                   std::to_string(manifest.usage.conversation_bytes) + ", tools " + std::to_string(manifest.usage.tool_bytes) +
                   ", opaque " + std::to_string(manifest.usage.opaque_bytes) + "\n";
    return description;
}

Result<session::ContextCheckpoint> ContextBuilder::take_checkpoint(provider::History history, const ContextManifest &manifest) const {
    const auto instruction_count = manifest.instructions.size();
    if (provider::instruction_prefix_size(history) != instruction_count) {
        return lighter::outcome_error(Error::protocol("provider operation changed the resolved instruction prefix"));
    }
    for (usize index = 0; index < instruction_count; ++index) {
        if (!matches_instruction(history[index], manifest.instructions[index])) {
            return lighter::outcome_error(Error::protocol("provider operation changed the resolved instruction prefix"));
        }
    }
    session::ContextCheckpoint checkpoint;
    checkpoint.items.reserve(history.size() - instruction_count);
    usize output_index = 0;
    for (usize index = instruction_count; index < history.size(); ++index) {
        auto &item = history[index];
        switch (item.role) {
            case provider::Role::USER: checkpoint.items.push_back(session::ContextInput{.parts = std::move(item.parts)}); break;
            case provider::Role::ASSISTANT: {
                std::vector<provider::TextPart> text;
                const auto next_id = [&] { return provider::OutputItemId{.value = "checkpoint:" + std::to_string(output_index++)}; };
                const auto flush_text = [&] {
                    if (text.empty()) return;
                    checkpoint.items.push_back(session::CheckpointOutput{.item = provider::AssistantMessageItem{
                                                                             .id = next_id(),
                                                                             .parts = std::exchange(text, {}),
                                                                             .phase = item.phase,
                                                                         }});
                };
                for (auto &part : item.parts) {
                    if (auto *value = std::get_if<provider::TextPart>(&part)) {
                        text.push_back(std::move(*value));
                    } else if (auto *value = std::get_if<provider::ToolCall>(&part)) {
                        flush_text();
                        checkpoint.items.push_back(session::CheckpointOutput{
                            .item = provider::ToolCallItem{.id = next_id(), .call = std::move(*value)},
                        });
                    } else if (auto *value = std::get_if<provider::OpaquePart>(&part)) {
                        flush_text();
                        checkpoint.items.push_back(session::CheckpointOutput{
                            .item = provider::ProviderOpaqueItem{.id = next_id(), .part = std::move(*value)},
                        });
                    } else {
                        return lighter::outcome_error(Error::protocol("assistant checkpoint contained a tool result"));
                    }
                }
                flush_text();
                break;
            }
            case provider::Role::SYSTEM:
            case provider::Role::DEVELOPER:
                return lighter::outcome_error(Error::protocol("provider compaction inserted instructions into conversation context"));
        }
    }
    return checkpoint;
}

} // namespace liminal::context
