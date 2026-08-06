#include "context.h"

#include <algorithm>
#include <concepts>
#include <type_traits>
#include <utility>

#include <lighter/async/vocab/outcome.h>

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
    return manifest;
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
