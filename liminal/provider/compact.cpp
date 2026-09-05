#include "compact.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <glaze/json.hpp>

#include <lighter/utils/enum.h>

#include <liminal/text.h>

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

constexpr usize k_bytes_per_token = 4;
/// Per-item framing charged on top of the text, mirroring the context
/// estimator (4 tokens per item plus 2 per part) for single-part items.
constexpr usize k_item_overhead_tokens = 6;

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

/// Renders one item as the plain text the summarizer sees. Tool calls and
/// results become prose because providers reject tool blocks in a request
/// that defines no tools (Anthropic answers with HTTP 400), and
/// provider-private parts are dropped because they carry no summarizable
/// content. This also keeps a trailing unmatched tool call from reaching the
/// wire as a dangling tool_use block. An item with no parts left is not sent.
Item render_for_summary(const Item &item) {
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
    return rendered;
}

/// The summarization request is a plain text conversation made of the
/// instruction prefix and the conversation items from `first` onward.
History textualize_for_summary(const History &history, usize instruction_count, usize first) {
    History plain;
    plain.reserve(instruction_count + history.size() - first);
    const auto render = [&plain](const Item &item) {
        auto rendered = render_for_summary(item);
        if (!rendered.parts.empty()) plain.push_back(std::move(rendered));
    };
    for (usize index = 0; index < instruction_count; ++index) render(history[index]);
    for (usize index = first; index < history.size(); ++index) render(history[index]);
    return plain;
}

/// A real user prompt is a user item made of text only (tool-result rounds
/// carry tool outcomes) that is not itself an earlier handoff summary.
std::optional<std::string> user_prompt_text(const Item &item) {
    if (item.role != Role::USER || item.parts.empty()) {
        return std::nullopt;
    }
    std::string text;
    for (const auto &part : item.parts) {
        const auto *value = std::get_if<TextPart>(&part);
        if (!value) return std::nullopt;
        if (!text.empty()) text += '\n';
        text += value->text;
    }
    if (text.starts_with(k_bridge_prefix)) {
        return std::nullopt;
    }
    return text;
}

usize approx_tokens(std::string_view text) { return (text.size() + k_bytes_per_token - 1) / k_bytes_per_token; }

/// Estimated tokens of one item as it reaches the summarizer, counted the way
/// the context estimator counts items. An item that renders to nothing costs
/// nothing: provider-private payloads never enter the request.
usize estimate_item_tokens(const Item &item) {
    const auto rendered = render_for_summary(item);
    if (rendered.parts.empty()) return 0;
    usize bytes = 0;
    for (const auto &part : rendered.parts) bytes += std::get<TextPart>(part).text.size();
    return (bytes + k_bytes_per_token - 1) / k_bytes_per_token + 4 + 2 * rendered.parts.size();
}

/// Recent user prompts travel into the compacted history verbatim so the
/// resuming model still sees what the user literally asked for. Selects the
/// newest ones that fit `token_budget`, truncating the oldest retained prompt
/// to fit, and returns them in transcript order.
std::vector<std::string> retained_prompts(const History &history, usize instruction_count, usize token_budget) {
    std::vector<std::string> prompts;
    usize remaining = token_budget;
    for (usize index = history.size(); index-- > instruction_count && remaining != 0;) {
        auto text = user_prompt_text(history[index]);
        if (!text) continue;
        const auto tokens = approx_tokens(*text) + k_item_overhead_tokens;
        if (tokens <= remaining) {
            remaining -= tokens;
            prompts.push_back(std::move(*text));
            continue;
        }
        if (remaining > k_item_overhead_tokens) {
            prompts.push_back(bounded_utf8(*text, (remaining - k_item_overhead_tokens) * k_bytes_per_token));
        }
        break;
    }
    std::ranges::reverse(prompts);
    return prompts;
}

} // namespace

Task<void, Error> local_compact(ProviderView provider, History &history, CompactRequest request) {
    const auto instruction_count = instruction_prefix_size(history);
    if (instruction_count == history.size()) {
        co_return;
    }

    std::string prompt(k_summarize_prompt);
    if (!request.instructions.empty()) {
        prompt += "\n\nAdditional instructions from the user:\n";
        prompt += request.instructions;
    }

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

    // The whole conversation is summarized, including an in-progress task:
    // sparing the latest task cannot shrink a long tool loop, which is exactly
    // when compaction is needed. The oldest conversation items are left out
    // up front when the estimate says the request would not fit the caller's
    // bound. If the provider still rejects it, cuts are probed exponentially
    // (dropping 1, 2, 4, ... more items) and the first accepted cut is used.
    // Rejections are cheap, the provider counts tokens and refuses before
    // generating, but every accepted probe generates a full, billable summary,
    // so refining toward a tighter cut would spend more than the recovered
    // items are worth. The newest item is always kept.
    usize first = instruction_count;
    if (request.summary_request_tokens) {
        usize estimated = estimate_item_tokens(Item{.role = Role::USER, .parts = {TextPart{.text = prompt}}});
        for (const auto &item : history) estimated += estimate_item_tokens(item);
        while (first + 1 < history.size() && estimated > *request.summary_request_tokens) {
            estimated -= estimate_item_tokens(history[first]);
            ++first;
        }
    }
    for (usize batch = 1;; batch *= 2) {
        auto summary_request = textualize_for_summary(history, instruction_count, first);
        append_user(summary_request, prompt);
        summary.clear();
        auto completion = co_await provider->complete(summary_request, {}, callbacks);
        std::optional<Error> rejection;
        if (!completion) {
            auto error = std::move(completion).error();
            if (error.code != ErrorCode::CONTEXT_EXHAUSTED) co_await fail(std::move(error));
            rejection = std::move(error);
        } else if ((*completion).stop == StopKind::CONTEXT_EXHAUSTED) {
            rejection = Error::protocol("compaction summary ended with " + (*completion).stop_detail);
        } else if ((*completion).stop != StopKind::DONE) {
            co_await fail(Error::protocol("compaction summary ended with " + (*completion).stop_detail));
        } else if (summary.empty()) {
            co_await fail(Error::protocol("compaction summary contained no text"));
        }
        if (!rejection) break;
        const auto droppable = history.size() - 1 - first;
        if (droppable == 0) co_await fail(std::move(*rejection));
        first += std::min(batch, droppable);
    }

    // With the summary known, retention is bounded once more by the target
    // size of the whole compacted history: instructions and summary are fixed
    // costs the target cannot shrink, prompts absorb whatever is left.
    std::string bridged = std::string(k_bridge_prefix) + summary;
    usize retention = request.retained_prompt_tokens;
    if (request.compacted_context_target_tokens) {
        usize fixed = approx_tokens(bridged) + k_item_overhead_tokens;
        for (usize index = 0; index < instruction_count; ++index) fixed += estimate_item_tokens(history[index]);
        retention =
            std::min(retention, *request.compacted_context_target_tokens > fixed ? *request.compacted_context_target_tokens - fixed : 0);
    }

    // Retained prompts precede the summary so the summary is the last thing
    // the resuming model reads before it continues.
    auto prompts = retained_prompts(history, instruction_count, retention);
    History compacted;
    compacted.reserve(instruction_count + prompts.size() + 1);
    compacted.insert(compacted.end(), std::make_move_iterator(history.begin()),
                     std::make_move_iterator(history.begin() + instruction_count));
    for (auto &text : prompts) append_user(compacted, std::move(text));
    compacted.push_back({.role = Role::USER, .parts = {TextPart{.text = std::move(bridged)}}});
    history = std::move(compacted);
}

} // namespace liminal::provider
