#include "compact.h"

#include <iterator>
#include <string>
#include <utility>
#include <variant>

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

/// A real user prompt (not a tool-result round) marks the start of a turn.
bool starts_turn(const Item &item) {
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
    if (history.empty()) {
        co_return;
    }

    // Cut at the start of the most recent turn, keeping that turn verbatim
    // (pi-style: cuts never split a tool_call/tool_result pair because pairs
    // live inside a turn). A single-turn history is summarized wholesale -
    // the caller asked for compaction, an unshrunk transcript would be a
    // silent no-op.
    usize cut = 0;
    for (usize index = history.size(); index-- > 0;) {
        if (starts_turn(history[index])) {
            cut = index;
            break;
        }
    }
    if (cut == 0) {
        cut = history.size();
    }

    History to_summarize(history.begin(), history.begin() + cut);
    std::string prompt(k_summarize_prompt);
    if (!instructions.empty()) {
        prompt += "\n\nAdditional instructions from the user:\n";
        prompt += instructions;
    }
    append_user(to_summarize, std::move(prompt));

    // No tools: the summarizer must answer in text, in one shot. Empty
    // callbacks keep the summary off the live console.
    auto response = co_await provider->complete(to_summarize, {}, {}).or_fail();
    if (response.stop != StopKind::DONE) {
        co_await fail(Error::protocol("compaction summary ended with " + response.stop_detail));
    }

    std::string summary;
    for (const auto &part : response.parts) {
        if (const auto *text = std::get_if<TextPart>(&part)) {
            summary += text->text;
        }
    }
    if (summary.empty()) {
        co_await fail(Error::protocol("compaction summary contained no text"));
    }

    History compacted;
    compacted.push_back({.role = Role::USER, .parts = {TextPart{.text = std::string(k_bridge_prefix) + summary}}});
    compacted.insert(compacted.end(), std::make_move_iterator(history.begin() + cut), std::make_move_iterator(history.end()));
    history = std::move(compacted);
}

} // namespace liminal::provider
