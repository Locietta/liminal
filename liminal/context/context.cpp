#include "context.h"

#include <algorithm>
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

bool matches_instruction(const provider::Item &item, const InstructionSource &source) {
    const auto expected_role = source.authority == InstructionAuthority::RUNTIME ? provider::Role::SYSTEM : provider::Role::DEVELOPER;
    if (item.role != expected_role || item.parts.size() != 1) {
        return false;
    }
    const auto *text = std::get_if<provider::TextPart>(&item.parts.front());
    return text && text->text == source.content;
}

} // namespace

Result<ContextManifest> ContextBuilder::build(std::span<const InstructionSource> sources, const provider::History &conversation) const {
    for (const auto &item : conversation) {
        if (provider::is_instruction(item.role)) {
            return lighter::outcome_error(
                Error::protocol("agent conversation contains an instruction; instructions must use sourced context"));
        }
    }

    std::vector<InstructionSource> ordered(sources.begin(), sources.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto &left, const auto &right) { return authority_rank(left.authority) < authority_rank(right.authority); });

    ContextManifest manifest;
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

    manifest.provider_history.reserve(manifest.instructions.size() + conversation.size());
    for (const auto &source : manifest.instructions) {
        append_instruction(manifest.provider_history, source);
    }
    manifest.provider_history.insert(manifest.provider_history.end(), conversation.begin(), conversation.end());
    return manifest;
}

Result<provider::History> ContextBuilder::take_conversation(provider::History history, const ContextManifest &manifest) const {
    const auto instruction_count = manifest.instructions.size();
    if (provider::instruction_prefix_size(history) != instruction_count) {
        return lighter::outcome_error(Error::protocol("provider operation changed the resolved instruction prefix"));
    }
    for (usize index = 0; index < instruction_count; ++index) {
        if (!matches_instruction(history[index], manifest.instructions[index])) {
            return lighter::outcome_error(Error::protocol("provider operation changed the resolved instruction prefix"));
        }
    }
    history.erase(history.begin(), history.begin() + instruction_count);
    return history;
}

} // namespace liminal::context
