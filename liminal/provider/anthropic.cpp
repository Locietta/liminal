#include "anthropic.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/async/async.h>
#include <lighter/async/io/watcher.h>
#include <lighter/codec/json/json.h>

#include <liminal/provider/compact.h>
#include <liminal/provider/detail/anthropic_protocol.h>
#include <liminal/provider/detail/completion_retry.h>

// Anthropic Messages API wire types. Internal: the public surface speaks the
// neutral transcript (provider/history.h); everything here is serialization
// detail behind complete()/compact().
namespace liminal::anthropic::wire {

struct TextBlock {
    std::string text;
    struct Citation {
        std::string type;
        std::string url;
        std::optional<std::string> title;
    };
    std::optional<std::vector<Citation>> citations;
};

// Neutral ToolCall's fields (id/name/input) already spell the Anthropic wire
// names, so it serializes as the tool_use block directly.
using ToolUseBlock = provider::ToolCall;

/// `tool_use_id` is Anthropic's spelling of the neutral ToolResult's
/// `call_id`. Requests only - responses never carry tool_result blocks.
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

struct ThinkingBlock {
    std::string thinking;
    std::string signature;
};

struct RedactedThinkingBlock {
    std::string data;
};

struct ServerToolUseBlock {
    std::string id;
    std::string name;
    glz::generic input;
};

struct WebSearchToolResultBlock {
    std::string tool_use_id;
    glz::generic content;
};

struct WebFetchToolResultBlock {
    std::string tool_use_id;
    glz::generic content;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock, RedactedThinkingBlock, ServerToolUseBlock,
                                  WebSearchToolResultBlock, WebFetchToolResultBlock>;

struct Tool {
    std::optional<std::string> type;
    std::string name;
    std::optional<std::string> description;
    std::optional<provider::InputSchema> input_schema;
};

struct Message {
    std::string role; // "user" / "assistant"
    std::vector<ContentBlock> content;
};

struct MessageRequest {
    struct Thinking {
        std::string type = "adaptive";
    };

    struct OutputConfig {
        std::string effort;
    };

    std::string model;
    u32 max_tokens = 8192;
    std::optional<std::string> system;
    std::vector<Message> messages;
    std::optional<std::vector<Tool>> tools; // omitted when nullopt
    bool stream = true;
    std::optional<Thinking> thinking;
    std::optional<OutputConfig> output_config;
};

struct Model {
    std::string id;
    std::string display_name;
};

struct ModelsResponse {
    std::vector<Model> data;
    bool has_more = false;
    std::optional<std::string> last_id;
};

struct Usage {
    u64 input_tokens = 0;
    u64 output_tokens = 0;
    u64 cache_creation_input_tokens = 0;
    u64 cache_read_input_tokens = 0;
};

struct AssistantMessage {
    std::string id;
    std::string model;
    std::vector<ContentBlock> content;
    std::string stop_reason;
    std::optional<std::string> stop_sequence;
    Usage usage;
    std::string request_id; // request-id response header
};

} // namespace liminal::anthropic::wire

template <>
struct glz::meta<liminal::anthropic::wire::ContentBlock> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"text",
                                           "tool_use",
                                           "tool_result",
                                           "thinking",
                                           "redacted_thinking",
                                           "server_tool_use",
                                           "web_search_tool_result",
                                           "web_fetch_tool_result"};
};

namespace liminal::anthropic {

namespace json = lighter::codec::json;
namespace http = lighter::http;
using lighter::fail;
using lighter::or_fail;
using lighter::outcome_error;
using lighter::Task;

namespace {

// The API adds fields over time; unknown keys must not break decoding.
inline constexpr json::Opts k_wire_opts{{.null_terminated = false, .error_on_unknown_keys = false}};

template <typename T>
Result<T> parse_wire(std::string_view text, std::string_view context) {
    T value{};
    if (auto ctx = glz::read<k_wire_opts>(value, text)) {
        return outcome_error(Error::json({.code = ctx.ec, .detail = glz::format_error(ctx, text)}, std::string(context)));
    }
    return value;
}

std::optional<std::string> citation_markdown(const wire::TextBlock::Citation &citation) {
    if (citation.url.empty()) return std::nullopt;
    const auto label = citation.title && !citation.title->empty() ? *citation.title : citation.url;
    return "[" + label + "](" + citation.url + ")";
}

std::string cited_text(wire::TextBlock text) {
    if (!text.citations || text.citations->empty()) return std::move(text.text);
    std::vector<std::string> citations;
    for (const auto &value : *text.citations) {
        auto citation = citation_markdown(value);
        if (citation && std::ranges::find(citations, *citation) == citations.end()) citations.push_back(*std::move(citation));
    }
    if (!citations.empty()) {
        text.text += "\n\nSources:";
        for (const auto &citation : citations) text.text += "\n- " + citation;
    }
    return std::move(text.text);
}

/// Anthropic envelope types that indicate a transient upstream condition.
bool transient_api_type(std::string_view type) {
    return type == "overloaded_error" || type == "rate_limit_error" || type == "api_error" || type == "timeout_error";
}

// --- neutral <-> wire translation ---------------------------------------

Result<std::string> encode_opaque(wire::ContentBlock block) {
    auto encoded = json::to_string(block);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "opaque block"));
    }
    return *std::move(encoded);
}

/// History -> wire messages. Opaque parts carrying our tag are replayed
/// verbatim (thinking blocks with their signatures); foreign-tagged parts
/// belong to another provider and are dropped.
struct WireHistory {
    std::optional<std::string> system;
    std::vector<wire::Message> messages;
};

void append_instruction(std::string &prompt, provider::Role role, std::string_view text) {
    if (text.empty()) return;
    if (prompt.empty()) {
        prompt = "Instruction hierarchy: SYSTEM instructions take precedence over DEVELOPER instructions.\n";
    }
    prompt += role == provider::Role::SYSTEM ? "\n[SYSTEM]\n" : "\n[DEVELOPER]\n";
    prompt += text;
}

Result<WireHistory> to_wire(const provider::History &history) {
    WireHistory serialized;
    serialized.messages.reserve(history.size());
    std::string system;
    bool conversation_started = false;

    for (const auto &item : history) {
        const bool instruction = provider::is_instruction(item.role);
        if (instruction && conversation_started) {
            return outcome_error(Error::protocol("system and developer instructions must precede conversation messages"));
        }
        if (instruction) {
            for (const auto &part : item.parts) {
                if (const auto *text = std::get_if<provider::TextPart>(&part)) {
                    append_instruction(system, item.role, text->text);
                } else {
                    return outcome_error(Error::protocol("instruction messages may contain only text"));
                }
            }
            continue;
        }
        conversation_started = true;

        wire::Message message{.role = item.role == provider::Role::ASSISTANT ? "assistant" : "user"};
        message.content.reserve(item.parts.size());

        for (const auto &part : item.parts) {
            if (const auto *text = std::get_if<provider::TextPart>(&part)) {
                message.content.push_back(wire::TextBlock{.text = text->text});
            } else if (const auto *call = std::get_if<provider::ToolCall>(&part)) {
                message.content.push_back(*call);
            } else if (const auto *result = std::get_if<provider::ToolResult>(&part)) {
                message.content.push_back(wire::ToolResultBlock{
                    .tool_use_id = result->call_id,
                    .content = result->content,
                    .is_error = result->is_error,
                });
            } else if (const auto *opaque = std::get_if<provider::OpaquePart>(&part)) {
                if (opaque->provider_tag != k_provider_tag) {
                    continue; // another provider's private state
                }
                auto block = parse_wire<wire::ContentBlock>(opaque->payload, "opaque replay");
                if (!block) {
                    return outcome_error(std::move(block).error());
                }
                message.content.push_back(*std::move(block));
            }
        }

        // An item whose parts were all foreign opaques would serialize as an
        // empty message, which the API rejects; skip it entirely.
        if (!message.content.empty()) {
            serialized.messages.push_back(std::move(message));
        }
    }
    if (!system.empty()) serialized.system = std::move(system);
    return serialized;
}

provider::StopKind to_stop_kind(std::string_view stop_reason) {
    if (stop_reason == "end_turn" || stop_reason == "stop_sequence") {
        return provider::StopKind::DONE;
    }
    if (stop_reason == "tool_use") {
        return provider::StopKind::NEEDS_TOOL_RESULTS;
    }
    if (stop_reason == "max_tokens") {
        return provider::StopKind::TRUNCATED;
    }
    if (stop_reason == "refusal") {
        return provider::StopKind::REFUSED;
    }
    if (stop_reason == "model_context_window_exceeded") {
        return provider::StopKind::CONTEXT_EXHAUSTED;
    }
    return provider::StopKind::OTHER;
}

/// Wire response -> neutral TurnResponse. Thinking blocks become opaque
/// parts so they replay bit-exact (signatures must not be re-encoded).
Result<provider::TurnResponse> to_turn_response(wire::AssistantMessage message) {
    const auto context_tokens = message.usage.input_tokens + message.usage.output_tokens + message.usage.cache_read_input_tokens +
                                message.usage.cache_creation_input_tokens;
    provider::TurnResponse response{
        .stop = to_stop_kind(message.stop_reason),
        .stop_detail = std::move(message.stop_reason),
        .usage = {.input_tokens = message.usage.input_tokens,
                  .output_tokens = message.usage.output_tokens,
                  .cache_read_tokens = message.usage.cache_read_input_tokens,
                  .cache_write_tokens = message.usage.cache_creation_input_tokens,
                  .context_tokens = context_tokens},
        .model = std::move(message.model),
        .request_id = std::move(message.request_id),
    };
    response.parts.reserve(message.content.size());

    for (auto &block : message.content) {
        if (auto *text = std::get_if<wire::TextBlock>(&block)) {
            response.parts.push_back(provider::TextPart{.text = cited_text(std::move(*text))});
        } else if (auto *call = std::get_if<wire::ToolUseBlock>(&block)) {
            response.parts.push_back(std::move(*call));
        } else if (std::holds_alternative<wire::ThinkingBlock>(block) || std::holds_alternative<wire::RedactedThinkingBlock>(block)) {
            auto payload = encode_opaque(std::move(block));
            if (!payload) {
                return outcome_error(std::move(payload).error());
            }
            response.parts.push_back(provider::OpaquePart{
                .provider_tag = std::string(k_provider_tag),
                .payload = *std::move(payload),
            });
        } else if (std::holds_alternative<wire::ServerToolUseBlock>(block) ||
                   std::holds_alternative<wire::WebSearchToolResultBlock>(block) ||
                   std::holds_alternative<wire::WebFetchToolResultBlock>(block)) {
            auto payload = encode_opaque(std::move(block));
            if (!payload) return outcome_error(std::move(payload).error());
            response.parts.push_back(provider::OpaquePart{
                .provider_tag = std::string(k_provider_tag),
                .payload = *std::move(payload),
            });
        } else {
            return outcome_error(Error::protocol("unexpected tool_result block in assistant response"));
        }
    }
    return response;
}

// --- SSE wire event payloads (probe shapes, all fields optional-ish) ----

struct WireUsage {
    std::optional<u64> input_tokens;
    std::optional<u64> output_tokens;
    std::optional<u64> cache_creation_input_tokens;
    std::optional<u64> cache_read_input_tokens;
};

struct WireMessageStart {
    std::string id;
    std::string model;
    std::optional<WireUsage> usage;
};

struct MessageStartEvent {
    WireMessageStart message;
};

struct WireBlockProbe {
    std::string type;
    std::optional<std::string> text;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
    std::optional<std::string> data;
    std::optional<std::string> tool_use_id;
    std::optional<glz::generic> content;
    std::optional<std::vector<wire::TextBlock::Citation>> citations;
};

struct ContentBlockStartEvent {
    u64 index = 0;
    WireBlockProbe content_block;
};

struct WireDeltaProbe {
    std::string type;
    std::optional<std::string> text;
    std::optional<std::string> partial_json;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
    std::optional<wire::TextBlock::Citation> citation;
};

struct ContentBlockDeltaEvent {
    u64 index = 0;
    WireDeltaProbe delta;
};

struct ContentBlockStopEvent {
    u64 index = 0;
};

struct WireMessageDelta {
    std::optional<std::string> stop_reason;
    std::optional<std::string> stop_sequence;
};

struct MessageDeltaEvent {
    WireMessageDelta delta;
    std::optional<WireUsage> usage;
};

struct WireApiError {
    std::string type;
    std::string message;
};

struct ErrorEvent {
    WireApiError error;
};

struct ApiErrorEnvelope {
    std::string type;
    WireApiError error;
    std::optional<std::string> request_id;
};

// --- stream accumulator -------------------------------------------------

struct PendingText {
    std::string text;
    std::vector<wire::TextBlock::Citation> citations;
};

struct PendingToolUse {
    std::string id;
    std::string name;
    std::string input_json;
};

struct PendingThinking {
    std::string thinking;
    std::string signature;
};

struct PendingServerToolUse {
    std::string id;
    std::string name;
    std::string input_json;
};

struct PendingServerResult {
    std::string type;
    std::string tool_use_id;
    glz::generic content;
};

using PendingBlock =
    std::variant<PendingText, PendingToolUse, PendingThinking, wire::RedactedThinkingBlock, PendingServerToolUse, PendingServerResult>;

struct StreamAccumulator {
    wire::AssistantMessage message;
    std::vector<std::optional<PendingBlock>> pending;
    std::vector<std::optional<wire::ContentBlock>> completed;

    bool saw_message_start = false;
    bool saw_message_stop = false;
    bool emitted_text = false;

    Result<void> consume(const http::sse::Event &event, const provider::StreamCallbacks &callbacks);
    Result<wire::AssistantMessage> finish() &&;

private:
    void apply_usage(const WireUsage &usage);
    Result<void> on_block_start(const ContentBlockStartEvent &event);
    Result<void> on_block_delta(const ContentBlockDeltaEvent &event, const provider::StreamCallbacks &callbacks);
    Result<void> on_block_stop(const ContentBlockStopEvent &event);
};

void StreamAccumulator::apply_usage(const WireUsage &usage) {
    // Streaming usage is cumulative: later events overwrite, never add.
    if (usage.input_tokens) message.usage.input_tokens = *usage.input_tokens;
    if (usage.output_tokens) message.usage.output_tokens = *usage.output_tokens;
    if (usage.cache_creation_input_tokens) {
        message.usage.cache_creation_input_tokens = *usage.cache_creation_input_tokens;
    }
    if (usage.cache_read_input_tokens) {
        message.usage.cache_read_input_tokens = *usage.cache_read_input_tokens;
    }
}

Result<void> StreamAccumulator::on_block_start(const ContentBlockStartEvent &event) {
    auto index = static_cast<usize>(event.index);
    if (pending.size() <= index) {
        pending.resize(index + 1);
        completed.resize(index + 1);
    }
    if (pending[index] || completed[index]) {
        return outcome_error(Error::protocol("content_block_start for an already-started index"));
    }

    const auto &probe = event.content_block;
    if (probe.type == "text") {
        pending[index] =
            PendingText{.text = probe.text.value_or(""), .citations = probe.citations.value_or(std::vector<wire::TextBlock::Citation>{})};
    } else if (probe.type == "tool_use") {
        pending[index] = PendingToolUse{.id = probe.id.value_or(""), .name = probe.name.value_or("")};
    } else if (probe.type == "server_tool_use") {
        pending[index] = PendingServerToolUse{.id = probe.id.value_or(""), .name = probe.name.value_or("")};
    } else if (probe.type == "web_search_tool_result" || probe.type == "web_fetch_tool_result") {
        pending[index] = PendingServerResult{
            .type = probe.type,
            .tool_use_id = probe.tool_use_id.value_or(""),
            .content = probe.content.value_or(glz::generic{}),
        };
    } else if (probe.type == "thinking") {
        pending[index] = PendingThinking{.thinking = probe.thinking.value_or(""), .signature = probe.signature.value_or("")};
    } else if (probe.type == "redacted_thinking") {
        pending[index] = wire::RedactedThinkingBlock{.data = probe.data.value_or("")};
    } else {
        return outcome_error(Error::protocol("unknown content block type: " + probe.type));
    }
    return {};
}

Result<void> StreamAccumulator::on_block_delta(const ContentBlockDeltaEvent &event, const provider::StreamCallbacks &callbacks) {
    auto index = static_cast<usize>(event.index);
    if (index >= pending.size() || !pending[index]) {
        return outcome_error(Error::protocol("content_block_delta without an active block"));
    }
    auto &block = *pending[index];
    const auto &delta = event.delta;

    if (delta.type == "text_delta") {
        auto *text = std::get_if<PendingText>(&block);
        if (!text) {
            return outcome_error(Error::protocol("text_delta for a non-text block"));
        }
        auto piece = delta.text.value_or("");
        text->text += piece;
        emitted_text = true;
        if (callbacks.on_text_delta && !piece.empty()) {
            callbacks.on_text_delta(piece);
        }
    } else if (delta.type == "input_json_delta") {
        if (auto *tool = std::get_if<PendingToolUse>(&block)) {
            tool->input_json += delta.partial_json.value_or("");
        } else if (auto *tool = std::get_if<PendingServerToolUse>(&block)) {
            tool->input_json += delta.partial_json.value_or("");
        } else {
            return outcome_error(Error::protocol("input_json_delta for a non-tool_use block"));
        }
    } else if (delta.type == "citations_delta") {
        auto *text = std::get_if<PendingText>(&block);
        if (!text || !delta.citation) return outcome_error(Error::protocol("citations_delta for a non-text block"));
        text->citations.push_back(*delta.citation);
        auto citation = citation_markdown(*delta.citation);
        if (citation && callbacks.on_text_delta) callbacks.on_text_delta("\n\nSource: " + *citation);
    } else if (delta.type == "thinking_delta") {
        auto *thinking = std::get_if<PendingThinking>(&block);
        if (!thinking) {
            return outcome_error(Error::protocol("thinking_delta for a non-thinking block"));
        }
        thinking->thinking += delta.thinking.value_or("");
    } else if (delta.type == "signature_delta") {
        auto *thinking = std::get_if<PendingThinking>(&block);
        if (!thinking) {
            return outcome_error(Error::protocol("signature_delta for a non-thinking block"));
        }
        thinking->signature += delta.signature.value_or("");
    } else {
        return outcome_error(Error::protocol("unknown delta type: " + delta.type));
    }
    return {};
}

Result<void> StreamAccumulator::on_block_stop(const ContentBlockStopEvent &event) {
    auto index = static_cast<usize>(event.index);
    if (index >= pending.size() || !pending[index]) {
        return outcome_error(Error::protocol("content_block_stop without an active block"));
    }

    auto block = *std::move(pending[index]);
    pending[index].reset();

    if (auto *text = std::get_if<PendingText>(&block)) {
        completed[index] = wire::TextBlock{
            .text = std::move(text->text),
            .citations = text->citations.empty() ? std::nullopt : std::optional(std::move(text->citations)),
        };
    } else if (auto *tool = std::get_if<PendingToolUse>(&block)) {
        std::string_view input_json = tool->input_json.empty() ? std::string_view("{}") : tool->input_json;
        auto input = parse_wire<glz::generic>(input_json, "tool_use input");
        if (!input) {
            return outcome_error(std::move(input).error());
        }
        if (!input->is_object()) {
            return outcome_error(Error::protocol("tool_use input is not a JSON object"));
        }
        completed[index] = wire::ToolUseBlock{
            .id = std::move(tool->id),
            .name = std::move(tool->name),
            .input = *std::move(input),
        };
    } else if (auto *thinking = std::get_if<PendingThinking>(&block)) {
        completed[index] = wire::ThinkingBlock{
            .thinking = std::move(thinking->thinking),
            .signature = std::move(thinking->signature),
        };
    } else if (auto *redacted = std::get_if<wire::RedactedThinkingBlock>(&block)) {
        completed[index] = std::move(*redacted);
    } else if (auto *tool = std::get_if<PendingServerToolUse>(&block)) {
        std::string_view input_json = tool->input_json.empty() ? std::string_view("{}") : tool->input_json;
        auto input = parse_wire<glz::generic>(input_json, "server_tool_use input");
        if (!input) return outcome_error(std::move(input).error());
        if (!input->is_object()) return outcome_error(Error::protocol("server_tool_use input is not a JSON object"));
        completed[index] = wire::ServerToolUseBlock{
            .id = std::move(tool->id),
            .name = std::move(tool->name),
            .input = *std::move(input),
        };
    } else if (auto *result = std::get_if<PendingServerResult>(&block)) {
        if (result->type == "web_search_tool_result") {
            completed[index] =
                wire::WebSearchToolResultBlock{.tool_use_id = std::move(result->tool_use_id), .content = std::move(result->content)};
        } else {
            completed[index] =
                wire::WebFetchToolResultBlock{.tool_use_id = std::move(result->tool_use_id), .content = std::move(result->content)};
        }
    }
    return {};
}

Result<void> StreamAccumulator::consume(const http::sse::Event &event, const provider::StreamCallbacks &callbacks) {
    const auto &name = event.event;

    if (name == "ping") {
        return {};
    }
    if (name == "message_start") {
        if (saw_message_start) {
            return outcome_error(Error::protocol("duplicate message_start"));
        }
        auto parsed = parse_wire<MessageStartEvent>(event.data, "message_start");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        saw_message_start = true;
        message.id = std::move(parsed->message.id);
        message.model = std::move(parsed->message.model);
        if (parsed->message.usage) {
            apply_usage(*parsed->message.usage);
        }
        return {};
    }
    if (!saw_message_start) {
        return outcome_error(Error::protocol("event before message_start: " + name));
    }
    if (name == "content_block_start") {
        auto parsed = parse_wire<ContentBlockStartEvent>(event.data, "content_block_start");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        return on_block_start(*parsed);
    }
    if (name == "content_block_delta") {
        auto parsed = parse_wire<ContentBlockDeltaEvent>(event.data, "content_block_delta");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        return on_block_delta(*parsed, callbacks);
    }
    if (name == "content_block_stop") {
        auto parsed = parse_wire<ContentBlockStopEvent>(event.data, "content_block_stop");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        return on_block_stop(*parsed);
    }
    if (name == "message_delta") {
        auto parsed = parse_wire<MessageDeltaEvent>(event.data, "message_delta");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        if (parsed->delta.stop_reason) {
            message.stop_reason = std::move(*parsed->delta.stop_reason);
        }
        if (parsed->delta.stop_sequence) {
            message.stop_sequence = std::move(*parsed->delta.stop_sequence);
        }
        if (parsed->usage) {
            apply_usage(*parsed->usage);
        }
        return {};
    }
    if (name == "message_stop") {
        for (const auto &entry : pending) {
            if (entry) {
                return outcome_error(Error::protocol("message_stop with unfinished content block"));
            }
        }
        if (message.stop_reason.empty()) {
            return outcome_error(Error::protocol("message_stop without a stop_reason"));
        }
        saw_message_stop = true;
        return {};
    }
    if (name == "error") {
        auto parsed = parse_wire<ErrorEvent>(event.data, "error event");
        if (!parsed) {
            return outcome_error(std::move(parsed).error());
        }
        bool transient = transient_api_type(parsed->error.type);
        return outcome_error(Error::api(std::move(parsed->error.type), std::move(parsed->error.message), message.request_id, transient));
    }
    // Unknown event types are ignored per Anthropic versioning policy.
    return {};
}

Result<wire::AssistantMessage> StreamAccumulator::finish() && {
    if (!saw_message_stop) {
        return outcome_error(Error::protocol("stream ended before message_stop"));
    }
    message.content.reserve(completed.size());
    for (auto &entry : completed) {
        if (!entry) {
            return outcome_error(Error::protocol("content block index gap in stream"));
        }
        message.content.push_back(*std::move(entry));
    }
    return std::move(message);
}

// --- request helpers ----------------------------------------------------

/// Drain up to `limit` bytes of an (error) response body; transport failures
/// just truncate what we report - we are already on an error path.
Task<std::string> read_bounded_body(http::StreamingResponse &response, usize limit) {
    std::string body;
    while (body.size() < limit) {
        auto chunk = co_await response.read_chunk();
        if (!chunk || chunk->empty()) {
            break;
        }
        auto take = std::min(chunk->size(), limit - body.size());
        body.append(chunk->data(), take);
        response.consume(chunk->size());
    }
    co_return body;
}

std::optional<std::chrono::milliseconds> parse_retry_after(const http::StreamingResponse &response) {
    auto value = response.header_value("retry-after");
    if (!value) {
        return std::nullopt;
    }
    char *end = nullptr;
    std::string text(*value);
    auto seconds = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || seconds < 0) {
        return std::nullopt;
    }
    return std::chrono::seconds(seconds);
}

Task<wire::AssistantMessage, Error> attempt_stream(http::Client &http_client, const ClientOptions &options, const std::string &body,
                                                   const provider::StreamCallbacks &callbacks, bool &text_emitted) {
    auto request = http_client.on().post(options.base_url + "/v1/messages");
    request.header("anthropic-version", "2023-06-01").header("accept", "text/event-stream").json_text(body);
    if (!options.auth) {
        co_await fail(Error::config("provider has no authentication resolver"));
    }
    auto auth = co_await options.auth().or_fail();
    if (!auth.api_key.empty()) {
        request.header("x-api-key", auth.api_key);
    }
    if (!auth.bearer_token.empty()) {
        request.bearer_auth(auth.bearer_token);
    }
    for (const auto &header : auth.headers) {
        request.header(header.name, header.value);
    }

    auto streamed = co_await std::move(request).stream();
    if (!streamed) {
        co_await fail(Error::http(std::move(streamed).error()));
    }
    auto response = *std::move(streamed);
    std::string request_id(response.header_value("request-id").value_or(""));

    if (!response.ok()) {
        auto error_body = co_await read_bounded_body(response, 64 * 1024);
        auto retry_after = parse_retry_after(response);
        std::string api_type;
        std::string detail = error_body;
        if (auto envelope = parse_wire<ApiErrorEnvelope>(error_body, "error body")) {
            api_type = std::move(envelope->error.type);
            detail = std::move(envelope->error.message);
            if (request_id.empty() && envelope->request_id) {
                request_id = std::move(*envelope->request_id);
            }
        }
        co_await fail(Error::http_status(response.status, std::move(api_type), std::move(detail), std::move(request_id), retry_after));
    }

    auto content_type = response.header_value("content-type").value_or("");
    if (content_type.find("text/event-stream") == std::string_view::npos) {
        co_await fail(Error::protocol("expected text/event-stream response, got: " + std::string(content_type)));
    }

    http::sse::EventStream events(std::move(response));
    StreamAccumulator accumulator;
    accumulator.message.request_id = std::move(request_id);

    while (!accumulator.saw_message_stop) {
        auto event = co_await events.next();
        if (!event) {
            text_emitted = accumulator.emitted_text;
            co_await fail(Error::http(std::move(event).error()));
        }
        if (!*event) {
            break; // clean EOF; finish() rejects it if message_stop never came
        }
        auto consumed = accumulator.consume(**event, callbacks);
        text_emitted = accumulator.emitted_text;
        if (!consumed) {
            co_await fail(std::move(consumed).error());
        }
    }

    co_return co_await or_fail(std::move(accumulator).finish());
}

} // namespace

namespace protocol {

Result<std::string> encode_complete_request(const provider::History &history, const std::vector<provider::ToolDefinition> &tools,
                                            const ClientOptions &options) {
    auto wire_history = to_wire(history);
    if (!wire_history) {
        return outcome_error(std::move(wire_history).error());
    }
    wire::MessageRequest request{
        .model = options.model,
        .max_tokens = options.max_tokens,
        .system = std::move(wire_history->system),
        .messages = std::move(wire_history->messages),
    };
    if (!tools.empty()) {
        std::vector<wire::Tool> encoded_tools;
        encoded_tools.reserve(tools.size());
        for (const auto &tool : tools) {
            switch (tool.kind) {
                case provider::ToolKind::FUNCTION:
                    encoded_tools.push_back({
                        .name = tool.name,
                        .description = tool.description,
                        .input_schema = tool.input_schema,
                    });
                    break;
                case provider::ToolKind::WEB_SEARCH: encoded_tools.push_back({.type = "web_search_20250305", .name = "web_search"}); break;
                case provider::ToolKind::WEB_FETCH: encoded_tools.push_back({.type = "web_fetch_20250910", .name = "web_fetch"}); break;
            }
        }
        request.tools = std::move(encoded_tools);
    }
    if (options.reasoning_effort) {
        request.thinking = wire::MessageRequest::Thinking{};
        request.output_config = wire::MessageRequest::OutputConfig{.effort = *options.reasoning_effort};
    }

    auto encoded = json::to_string(request);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "request body"));
    }
    return *std::move(encoded);
}

Result<provider::TurnResponse> decode_stream(std::span<const http::sse::Event> events, const provider::StreamCallbacks &callbacks,
                                             std::string request_id) {
    StreamAccumulator accumulator;
    accumulator.message.request_id = std::move(request_id);
    for (const auto &event : events) {
        auto consumed = accumulator.consume(event, callbacks);
        if (!consumed) {
            return outcome_error(std::move(consumed).error());
        }
    }
    auto message = std::move(accumulator).finish();
    if (!message) {
        return outcome_error(std::move(message).error());
    }
    return to_turn_response(*std::move(message));
}

} // namespace protocol

Client::Client(ClientOptions options) : options(std::move(options)) {
    while (!this->options.base_url.empty() && this->options.base_url.back() == '/') {
        this->options.base_url.pop_back();
    }
}

Task<provider::TurnResponse, Error> Client::complete(const provider::History &history, const std::vector<provider::ToolDefinition> &tools,
                                                     const provider::StreamCallbacks &callbacks) {
    const std::string body = co_await or_fail(protocol::encode_complete_request(history, tools, options));
    provider::detail::CompletionAttempts attempts{
        .stream = [this](const std::string &request_body, const provider::StreamCallbacks &stream_callbacks,
                         bool &text_emitted) -> Task<provider::TurnResponse, Error> {
            auto message = co_await attempt_stream(http_client, options, request_body, stream_callbacks, text_emitted).or_fail();
            co_return co_await or_fail(to_turn_response(std::move(message)));
        },
        .sleep = [](std::chrono::milliseconds delay) { return lighter::sleep(delay); },
    };
    co_return co_await provider::detail::complete_with_retry(attempts, body, callbacks, options.max_retries, options.initial_retry_delay)
        .or_fail();
}

Task<void, Error> Client::compact(provider::History &history, std::string_view instructions) {
    // No native compaction endpoint; summarize through our own complete().
    co_return co_await provider::local_compact(this, history, instructions).or_fail();
}

Task<std::vector<provider::DiscoveredModel>, Error> list_models(ClientOptions options) {
    while (!options.base_url.empty() && options.base_url.back() == '/') {
        options.base_url.pop_back();
    }

    http::Client client;
    std::vector<provider::DiscoveredModel> models;
    std::optional<std::string> after_id;
    do {
        auto request = client.on().get(options.base_url + "/v1/models");
        request.header("anthropic-version", "2023-06-01").query("limit", "1000");
        if (after_id) {
            request.query("after_id", *after_id);
        }
        if (!options.auth) {
            co_await fail(Error::config("provider has no authentication resolver"));
        }
        auto auth = co_await options.auth().or_fail();
        if (!auth.api_key.empty()) {
            request.header("x-api-key", auth.api_key);
        }
        if (!auth.bearer_token.empty()) {
            request.bearer_auth(auth.bearer_token);
        }
        for (const auto &header : auth.headers) {
            request.header(header.name, header.value);
        }

        auto sent = co_await std::move(request).send();
        if (!sent) {
            co_await fail(Error::http(std::move(sent).error()));
        }
        if (!sent->ok()) {
            auto request_id = std::string(sent->header_value("request-id").value_or(""));
            co_await fail(Error::http_status(sent->status, {}, sent->text_copy(), std::move(request_id)));
        }
        auto response = parse_wire<wire::ModelsResponse>(sent->text(), "models response");
        if (!response) {
            co_await fail(std::move(response).error());
        }
        for (auto &entry : response->data) {
            if (!entry.id.empty()) {
                models.push_back({.id = std::move(entry.id), .name = std::move(entry.display_name)});
            }
        }
        if (!response->has_more) {
            break;
        }
        if (!response->last_id || response->last_id->empty() || response->last_id == after_id) {
            co_await fail(Error::protocol("models response pagination did not advance"));
        }
        after_id = std::move(response->last_id);
    } while (true);

    co_return models;
}

} // namespace liminal::anthropic
