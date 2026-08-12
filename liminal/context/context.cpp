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

void append_entry(provider::History &history, const session::SessionEntry &entry, std::optional<UsageBaseline> *baseline = nullptr) {
    std::visit(
        [&history, baseline](const auto &payload) {
            using T = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<T, session::TaskStarted>) {
                provider::append_user(history, payload.text);
            } else if constexpr (std::same_as<T, session::OutputItemCompleted>) {
                provider::append_output_item(history, payload.item);
            } else if constexpr (std::same_as<T, session::ProviderCallCompleted>) {
                if (baseline && payload.completion.usage.context_tokens != 0) {
                    *baseline =
                        UsageBaseline{.history_size = history.size(), .context_tokens = to_usize(payload.completion.usage.context_tokens)};
                }
            } else if constexpr (std::same_as<T, session::ToolResults>) {
                provider::append_tool_results(history, payload.results);
            } else if constexpr (std::same_as<T, session::ContextCheckpoint>) {
                for (const auto &item : payload.items) {
                    append_checkpoint_item(history, item);
                }
            }
        },
        entry.payload);
}

bool starts_semantic_turn(const session::SessionEntry &entry) {
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
            } else if constexpr (std::same_as<T, provider::ToolResult>) {
                return value.call_id.size() + value.content.size();
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
            } else if (std::holds_alternative<provider::ToolCall>(part) || std::holds_alternative<provider::ToolResult>(part)) {
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
                                              ContextBudget budget) const {
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
            if (index == start || starts_semantic_turn(*branch[index])) {
                unit_starts.push_back(index);
            }
        }
        for (auto unit = unit_starts.rbegin(); unit != unit_starts.rend(); ++unit) {
            auto candidate = instruction_history;
            std::optional<UsageBaseline> baseline;
            for (usize index = *unit; index < branch.size(); ++index) {
                append_entry(candidate, *branch[index], *unit == start ? &baseline : nullptr);
            }
            const auto candidate_usage = estimate_usage(candidate, budget, baseline);
            if (*candidate_usage.remaining_input_tokens < 0) {
                if (unit == unit_starts.rbegin()) {
                    return lighter::outcome_error(Error::protocol("the latest semantic turn exceeds the model input budget"));
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
    for (usize index = selected_start; index < branch.size(); ++index) {
        manifest.session_entries.push_back(branch[index]->id);
        append_entry(manifest.provider_history, *branch[index], selected_start == start ? &baseline : nullptr);
    }
    manifest.usage = estimate_usage(manifest.provider_history, manifest.budget, baseline);
    return manifest;
}

std::string describe(const ContextManifest &manifest) {
    std::string description = "context\n";
    description += "session: " + std::to_string(manifest.session_id.value) + "\n";
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
