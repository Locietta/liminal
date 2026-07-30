#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/types.hpp>

#include "liminal/provider/common.h"

namespace liminal::provider {

using namespace lighter::types;

// Provider-neutral transcript. This is the history liminal owns; concrete
// clients translate it to/from their wire formats on every request. Provider
// switching, persistence, and compaction all operate on this representation.

enum struct Role {
    USER,
    ASSISTANT,
};

struct TextPart {
    std::string text;
};

/// Provider-private state carried opaquely through the transcript: thinking
/// blocks + signatures (Anthropic), encrypted reasoning and compaction items
/// (OpenAI), ... Only the provider whose `provider_tag` matches interprets
/// `payload` (its own serialized wire form); everyone else preserves it
/// verbatim and drops it when replaying to a *different* provider.
struct OpaquePart {
    std::string provider_tag;
    std::string payload;
};

using Part = std::variant<TextPart, ToolCall, ToolResult, OpaquePart>;

struct Item {
    Role role = Role::USER;
    std::vector<Part> parts;
};

using History = std::vector<Item>;

/// Why a completion stopped, normalized across providers. Unknown
/// provider-native reasons map to OTHER with the raw reason in
/// TurnResponse::stop_detail; the agent decides how to react.
enum struct StopKind {
    DONE,               ///< terminal response, commit and return to the user
    NEEDS_TOOL_RESULTS, ///< model requested tool calls; continue the turn
    TRUNCATED,          ///< output cut off (max_tokens / incomplete)
    REFUSED,            ///< model refused to answer
    CONTEXT_EXHAUSTED,  ///< conversation no longer fits the context window
    OTHER,              ///< unrecognized provider stop reason (see stop_detail)
};

struct Usage {
    u64 input_tokens = 0;
    u64 output_tokens = 0;
    u64 cache_read_tokens = 0;
    u64 cache_write_tokens = 0;
    u64 reasoning_tokens = 0;
};

/// One assistant response, already translated to neutral parts.
struct TurnResponse {
    std::vector<Part> parts;
    StopKind stop = StopKind::OTHER;
    std::string stop_detail; ///< provider-native stop reason, for diagnostics
    Usage usage;
    std::string model;
    std::string request_id;
};

// --- history manipulation ----------------------------------------------

inline void append_user(History &history, std::string prompt) {
    history.push_back({.role = Role::USER, .parts = {TextPart{.text = std::move(prompt)}}});
}

inline void append_response(History &history, TurnResponse response) {
    history.push_back({.role = Role::ASSISTANT, .parts = std::move(response.parts)});
}

/// All tool results of one round travel in a single user item, in call order.
inline void append_tool_results(History &history, std::vector<ToolResult> results) {
    Item item{.role = Role::USER};
    item.parts.reserve(results.size());
    for (auto &result : results) {
        item.parts.push_back(std::move(result));
    }
    history.push_back(std::move(item));
}

inline std::vector<const ToolCall *> tool_calls(const TurnResponse &response) {
    std::vector<const ToolCall *> calls;
    for (const auto &part : response.parts) {
        if (const auto *call = std::get_if<ToolCall>(&part)) {
            calls.push_back(call);
        }
    }
    return calls;
}

} // namespace liminal::provider

// Tagged so the neutral transcript can be persisted/debugged as JSON(L).
template <>
struct glz::meta<liminal::provider::Part> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"text", "tool_call", "tool_result", "opaque"};
};
