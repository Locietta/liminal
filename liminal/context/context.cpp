#include "context.h"

#include <algorithm>
#include <concepts>
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
            } else if constexpr (std::same_as<T, session::AgentOutput>) {
                history.push_back({.role = provider::Role::ASSISTANT, .parts = value.parts});
            }
        },
        item);
}

void append_entry(provider::History &history, const session::SessionEntry &entry) {
    std::visit(
        [&history](const auto &payload) {
            using T = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<T, session::UserMessage>) {
                provider::append_user(history, payload.text);
            } else if constexpr (std::same_as<T, session::AgentOutput>) {
                history.push_back({.role = provider::Role::ASSISTANT, .parts = payload.parts});
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

bool matches_instruction(const provider::Item &item, const InstructionSource &source) {
    const auto expected_role = source.authority == InstructionAuthority::RUNTIME ? provider::Role::SYSTEM : provider::Role::DEVELOPER;
    if (item.role != expected_role || item.parts.size() != 1) {
        return false;
    }
    const auto *text = std::get_if<provider::TextPart>(&item.parts.front());
    return text && text->text == source.content;
}

usize estimated_part_bytes(const provider::Part &part) {
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

usize estimated_history_bytes(const provider::History &history) {
    usize size = 0;
    for (const auto &item : history) {
        for (const auto &part : item.parts) {
            size += estimated_part_bytes(part);
        }
    }
    return size;
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

Result<ContextManifest> ContextBuilder::build(std::span<const InstructionSource> sources, const session::Session &session) const {
    std::vector<InstructionSource> ordered(sources.begin(), sources.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto &left, const auto &right) { return authority_rank(left.authority) < authority_rank(right.authority); });

    ContextManifest manifest{.session_id = session.id};
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
    manifest.omitted_session_entries = start;
    manifest.session_entries.reserve(branch.size() - start);

    manifest.provider_history.reserve(manifest.instructions.size() + branch.size() - start);
    for (const auto &source : manifest.instructions) {
        append_instruction(manifest.provider_history, source);
    }
    for (usize index = start; index < branch.size(); ++index) {
        manifest.session_entries.push_back(branch[index]->id);
        append_entry(manifest.provider_history, *branch[index]);
    }
    manifest.estimated_context_bytes = estimated_history_bytes(manifest.provider_history);
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
    description += "\nsession entries omitted: " + std::to_string(manifest.omitted_session_entries) + "\n";
    description += manifest.active_checkpoint ? "active checkpoint: " + std::to_string(manifest.active_checkpoint->value) + "\n" :
                                                "active checkpoint: none\n";
    description += "estimated context payload: " + std::to_string(manifest.estimated_context_bytes) + " bytes\n";
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
    for (usize index = instruction_count; index < history.size(); ++index) {
        auto &item = history[index];
        switch (item.role) {
            case provider::Role::USER: checkpoint.items.push_back(session::ContextInput{.parts = std::move(item.parts)}); break;
            case provider::Role::ASSISTANT: checkpoint.items.push_back(session::AgentOutput{.parts = std::move(item.parts)}); break;
            case provider::Role::SYSTEM:
            case provider::Role::DEVELOPER:
                return lighter::outcome_error(Error::protocol("provider compaction inserted instructions into conversation context"));
        }
    }
    return checkpoint;
}

} // namespace liminal::context
