#include "compact.h"

#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <glaze/json.hpp>

#include <lighter/utils/enum.h>

namespace liminal::provider {

using lighter::fail;
using lighter::Task;

namespace {

/// Summarization request appended as the final user message (codex-style
/// local compaction: the model writes a handoff summary for its successor).
constexpr std::string_view k_summarize_prompt =
    "You are performing a context checkpoint compaction. Write a handoff summary for another "
    "assistant that will resume this conversation with no other context. Include: the user's goals "
    "and constraints, decisions made and why, work completed so far (with exact file paths), "
    "important tool results, unresolved issues, and concrete next steps.";

/// Prefixed to the stored summary so the resuming model knows what it reads.
constexpr std::string_view k_bridge_prefix = "[Compacted context] Another assistant worked on this conversation and produced the handoff "
                                             "summary below. Build on that work; do not redo it.\n\n";

/// Renders a tool call as prose for the summarizer.
std::string describe_tool_call(const ToolCall &call) {
    std::string text = "[tool call " + call.name + " #" + call.id + "]";
    if (auto input = glz::write_json(call.input); input && !input->empty() && *input != "null") {
        text += '\n';
        text += *input;
    }
    return text;
}

/// Renders a tool result as prose for the summarizer.
std::string describe_tool_outcome(const ToolOutcome &outcome) {
    std::string text = "[tool result #" + outcome.call_id + ": " + std::string(lighter::enum_name(outcome.kind)) + "]";
    if (!outcome.receipt.empty()) {
        text += '\n';
        text += outcome.receipt;
    }
    if (!outcome.payload.empty()) {
        text += '\n';
        text += outcome.payload;
    }
    return text;
}

/// The summarization request is a plain text conversation. Tool calls and
/// results are rendered as prose because providers reject tool blocks in a
/// request that defines no tools (Anthropic answers with HTTP 400), and
/// provider-private parts are dropped because they carry no summarizable
/// content. This also keeps a trailing unmatched tool call from reaching the
/// wire as a dangling tool_use block.
History textualize_for_summary(const History &history, usize count) {
    History plain;
    plain.reserve(count);
    for (usize index = 0; index < count; ++index) {
        const auto &item = history[index];
        Item rendered{.role = item.role, .phase = item.phase};
        for (const auto &part : item.parts) {
            std::visit(
                [&rendered](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, TextPart>) {
                        rendered.parts.push_back(value);
                    } else if constexpr (std::is_same_v<T, ToolCall>) {
                        rendered.parts.push_back(TextPart{.text = describe_tool_call(value)});
                    } else if constexpr (std::is_same_v<T, ToolOutcome>) {
                        rendered.parts.push_back(TextPart{.text = describe_tool_outcome(value)});
                    }
                },
                part);
        }
        if (!rendered.parts.empty()) plain.push_back(std::move(rendered));
    }
    return plain;
}

/// A real user prompt (not a tool-result round) marks the start of a task.
bool starts_task(const Item &item) {
    if (item.role != Role::USER) {
        return false;
    }
    for (const auto &part : item.parts) {
        if (std::holds_alternative<TextPart>(part)) {
            return true;
        }
    }
    return false;
}

} // namespace

Task<void, Error> local_compact(ProviderView provider, History &history, std::string_view instructions) {
    const auto instruction_count = instruction_prefix_size(history);
    if (instruction_count == history.size()) {
        co_return;
    }

    // Cut at the start of the most recent task, keeping that task verbatim
    // (pi-style: cuts never split a tool_call/tool_result pair because pairs
    // live inside a task). A single-task history is summarized wholesale -
    // the caller asked for compaction, an unshrunk transcript would be a
    // silent no-op.
    usize cut = instruction_count;
    for (usize index = history.size(); index-- > 0;) {
        if (starts_task(history[index])) {
            cut = index;
            break;
        }
    }
    if (cut == instruction_count) {
        cut = history.size();
    }

    History to_summarize = textualize_for_summary(history, cut);
    std::string prompt(k_summarize_prompt);
    if (!instructions.empty()) {
        prompt += "\n\nAdditional instructions from the user:\n";
        prompt += instructions;
    }
    append_user(to_summarize, std::move(prompt));

    // No tools: the summarizer must answer in text, in one shot. Empty
    // callbacks keep the summary off the live console.
    std::string summary;
    StreamCallbacks callbacks{
        .on_item_completed =
            [&summary](const OutputItem &item) {
                if (const auto *message = std::get_if<AssistantMessageItem>(&item)) {
                    for (const auto &part : message->parts) summary += part.text;
                }
            },
    };
    auto completion = co_await provider->complete(to_summarize, {}, callbacks).or_fail();
    if (completion.stop != StopKind::DONE) {
        co_await fail(Error::protocol("compaction summary ended with " + completion.stop_detail));
    }
    if (summary.empty()) {
        co_await fail(Error::protocol("compaction summary contained no text"));
    }

    History compacted;
    compacted.reserve(instruction_count + 1 + history.size() - cut);
    compacted.insert(compacted.end(), std::make_move_iterator(history.begin()),
                     std::make_move_iterator(history.begin() + instruction_count));
    compacted.push_back({.role = Role::USER, .parts = {TextPart{.text = std::string(k_bridge_prefix) + summary}}});
    compacted.insert(compacted.end(), std::make_move_iterator(history.begin() + cut), std::make_move_iterator(history.end()));
    history = std::move(compacted);
}

} // namespace liminal::provider
