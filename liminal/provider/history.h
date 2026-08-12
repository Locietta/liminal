#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/types.hpp>

#include <liminal/provider/common.h>

namespace liminal::provider {

using namespace lighter::types;

// Provider request projection. ContextBuilder lowers Liminal's semantic
// instructions and session entries into this representation; concrete clients
// then translate it to/from their wire formats. Persistence and branching
// operate on session::Session instead.

enum struct Role {
    SYSTEM,
    DEVELOPER,
    USER,
    ASSISTANT,
};

struct TextPart {
    std::string text;
};

/// Provider-private state carried opaquely through semantic session entries
/// and request projections: thinking blocks + signatures (Anthropic),
/// encrypted reasoning and compaction items (OpenAI), ... Only the provider
/// whose `provider_tag` matches interprets `payload` (its own serialized wire
/// form); everyone else preserves it verbatim and drops it when replaying to a
/// *different* provider.
struct OpaquePart {
    std::string provider_tag;
    std::string payload;
};

using Part = std::variant<TextPart, ToolCall, ToolResult, OpaquePart>;

enum struct MessagePhase {
    UNSPECIFIED,
    COMMENTARY,
    FINAL,
};

struct Item {
    Role role = Role::USER;
    std::vector<Part> parts;
    MessagePhase phase = MessagePhase::UNSPECIFIED;
};

using History = std::vector<Item>;

struct OutputItemId {
    std::string value;
    auto operator<=>(const OutputItemId &) const = default;
};

enum struct OutputItemKind {
    ASSISTANT_MESSAGE,
    TOOL_CALL,
    PROVIDER_OPAQUE,
};

struct OutputItemHeader {
    OutputItemId id;
    OutputItemKind kind = OutputItemKind::PROVIDER_OPAQUE;
    MessagePhase phase = MessagePhase::UNSPECIFIED;
};

struct AssistantMessageItem {
    OutputItemId id;
    std::vector<TextPart> parts;
    MessagePhase phase = MessagePhase::UNSPECIFIED;
};

struct ToolCallItem {
    OutputItemId id;
    ToolCall call;
};

struct ProviderOpaqueItem {
    OutputItemId id;
    OpaquePart part;
};

using OutputItem = std::variant<AssistantMessageItem, ToolCallItem, ProviderOpaqueItem>;

struct StreamCallbacks {
    std::copyable_function<void(const OutputItemHeader &) const> on_item_started;
    std::copyable_function<void(const OutputItemId &, std::string_view) const> on_assistant_text_delta;
    std::copyable_function<void(const OutputItem &) const> on_item_completed;
};

/// Why a provider call stopped, normalized across providers. Unknown
/// provider-native reasons map to OTHER with the raw reason in
/// ProviderCallCompletion::stop_detail; the agent decides how to react.
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
    /// Provider-normalized model context immediately after this response.
    /// This includes the request input and generated output without double
    /// counting provider-specific cache accounting fields.
    u64 context_tokens = 0;
};

/// Metadata returned once a provider call has finished. Completed output is
/// delivered incrementally through StreamCallbacks instead of being batched
/// into this value.
struct ProviderCallCompletion {
    StopKind stop = StopKind::OTHER;
    std::string stop_detail; ///< provider-native stop reason, for diagnostics
    Usage usage;
    std::string model;
    std::string request_id;
};

// --- history manipulation ----------------------------------------------

inline bool is_instruction(Role role) { return role == Role::SYSTEM || role == Role::DEVELOPER; }

/// System and developer instructions form an immutable leading prefix. This
/// is the strongest representation portable across the Responses API and the
/// Anthropic Messages API.
inline usize instruction_prefix_size(const History &history) {
    usize size = 0;
    while (size < history.size() && is_instruction(history[size].role)) ++size;
    return size;
}

inline void append_system(History &history, std::string instructions) {
    history.push_back({.role = Role::SYSTEM, .parts = {TextPart{.text = std::move(instructions)}}});
}

inline void append_developer(History &history, std::string instructions) {
    history.push_back({.role = Role::DEVELOPER, .parts = {TextPart{.text = std::move(instructions)}}});
}

inline void append_user(History &history, std::string prompt) {
    history.push_back({.role = Role::USER, .parts = {TextPart{.text = std::move(prompt)}}});
}

inline void append_output_item(History &history, const OutputItem &output) {
    std::visit(
        [&history](const auto &item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, AssistantMessageItem>) {
                Item history_item{.role = Role::ASSISTANT, .phase = item.phase};
                history_item.parts.reserve(item.parts.size());
                for (const auto &part : item.parts) history_item.parts.push_back(part);
                history.push_back(std::move(history_item));
            } else if constexpr (std::is_same_v<T, ToolCallItem>) {
                history.push_back({.role = Role::ASSISTANT, .parts = {item.call}});
            } else {
                history.push_back({.role = Role::ASSISTANT, .parts = {item.part}});
            }
        },
        output);
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

} // namespace liminal::provider

// Tagged so semantic session payloads and provider projections can be encoded.
template <>
struct glz::meta<liminal::provider::Part> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"text", "tool_call", "tool_result", "opaque"};
};

template <>
struct glz::meta<liminal::provider::OutputItem> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"assistant_message", "tool_call", "provider_opaque"};
};
