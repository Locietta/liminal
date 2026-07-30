#include "anthropic.h"

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
#include <lighter/utils/panic.h>

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

using PendingBlock = std::variant<PendingText, PendingToolUse, PendingThinking, RedactedThinkingBlock>;

struct StreamAccumulator {
    AssistantMessage message;
    std::vector<std::optional<PendingBlock>> pending;
    std::vector<std::optional<ContentBlock>> completed;

    bool saw_message_start = false;
    bool saw_message_stop = false;
    bool emitted_text = false;

    Result<void> consume(const http::sse::Event &event, const StreamCallbacks &callbacks);
    Result<AssistantMessage> finish() &&;

private:
    void apply_usage(const WireUsage &usage);
    Result<void> on_block_start(const ContentBlockStartEvent &event);
    Result<void> on_block_delta(const ContentBlockDeltaEvent &event, const StreamCallbacks &callbacks);
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
        pending[index] = PendingText{.text = probe.text.value_or("")};
    } else if (probe.type == "tool_use") {
        pending[index] = PendingToolUse{.id = probe.id.value_or(""), .name = probe.name.value_or("")};
    } else if (probe.type == "thinking") {
        pending[index] = PendingThinking{.thinking = probe.thinking.value_or(""), .signature = probe.signature.value_or("")};
    } else if (probe.type == "redacted_thinking") {
        pending[index] = RedactedThinkingBlock{.data = probe.data.value_or("")};
    } else {
        return outcome_error(Error::protocol("unknown content block type: " + probe.type));
    }
    return {};
}

Result<void> StreamAccumulator::on_block_delta(const ContentBlockDeltaEvent &event, const StreamCallbacks &callbacks) {
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
        auto *tool = std::get_if<PendingToolUse>(&block);
        if (!tool) {
            return outcome_error(Error::protocol("input_json_delta for a non-tool_use block"));
        }
        tool->input_json += delta.partial_json.value_or("");
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
        completed[index] = TextBlock{.text = std::move(text->text)};
    } else if (auto *tool = std::get_if<PendingToolUse>(&block)) {
        std::string_view input_json = tool->input_json.empty() ? std::string_view("{}") : tool->input_json;
        auto input = parse_wire<glz::generic>(input_json, "tool_use input");
        if (!input) {
            return outcome_error(std::move(input).error());
        }
        if (!input->is_object()) {
            return outcome_error(Error::protocol("tool_use input is not a JSON object"));
        }
        completed[index] = ToolUseBlock{
            .id = std::move(tool->id),
            .name = std::move(tool->name),
            .input = *std::move(input),
        };
    } else if (auto *thinking = std::get_if<PendingThinking>(&block)) {
        completed[index] = ThinkingBlock{
            .thinking = std::move(thinking->thinking),
            .signature = std::move(thinking->signature),
        };
    } else if (auto *redacted = std::get_if<RedactedThinkingBlock>(&block)) {
        completed[index] = std::move(*redacted);
    }
    return {};
}

Result<void> StreamAccumulator::consume(const http::sse::Event &event, const StreamCallbacks &callbacks) {
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
        return outcome_error(Error::http_status(0, std::move(parsed->error.type), std::move(parsed->error.message), message.request_id));
    }
    // Unknown event types are ignored per Anthropic versioning policy.
    return {};
}

Result<AssistantMessage> StreamAccumulator::finish() && {
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

Task<AssistantMessage, Error> attempt_stream(http::Client &http_client, const ClientOptions &options, const std::string &body,
                                             const StreamCallbacks &callbacks, bool &text_emitted) {
    auto request = http_client.on().post(options.base_url + "/v1/messages");
    request.header("anthropic-version", "2023-06-01").header("accept", "text/event-stream").json_text(body);
    if (!options.api_key.empty()) {
        request.header("x-api-key", options.api_key);
    }
    if (!options.auth_token.empty()) {
        request.bearer_auth(options.auth_token);
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

Client::Client(ClientOptions options) : options(std::move(options)) {
    while (!this->options.base_url.empty() && this->options.base_url.back() == '/') {
        this->options.base_url.pop_back();
    }
}

Task<AssistantMessage, Error> Client::create_message(const MessageRequest &request, const StreamCallbacks &callbacks) {
    auto encoded = json::to_string(request);
    if (!encoded) {
        co_await fail(Error::json(std::move(encoded).error(), "request body"));
    }
    const std::string body = *std::move(encoded);

    bool text_emitted = false;
    for (usize attempt = 0;; ++attempt) {
        auto outcome = co_await attempt_stream(http_client, options, body, callbacks, text_emitted);
        if (outcome) {
            co_return *std::move(outcome);
        }
        auto error = std::move(outcome).error();

        // Once output reached the user we cannot transparently re-send:
        // the duplicated prefix would be visible. Surface the error instead.
        bool can_retry = attempt < options.max_retries && error.retryable() && !text_emitted;
        if (!can_retry) {
            co_await fail(std::move(error));
        }

        auto delay = error.retry_after.value_or(options.initial_retry_delay * (1 << attempt));
        co_await lighter::sleep(delay);
    }
}

Task<AssistantMessage, Error> Client::create_message(std::string model, u32 max_tokens, const History &history,
                                                     const std::vector<ToolDefinition> &tools, const StreamCallbacks &callbacks) {
    co_return co_await create_message(
        {
            .model = std::move(model),
            .max_tokens = max_tokens,
            .messages = history,
            .tools = tools,
        },
        callbacks);
}

void Client::append_user(History &history, std::string prompt) { history.push_back(user_text(std::move(prompt))); }

std::vector<const ToolUseBlock *> Client::tool_calls(const Response &response) {
    std::vector<const ToolUseBlock *> calls;
    for (const auto &block : response.content) {
        if (const auto *call = std::get_if<ToolUseBlock>(&block)) {
            calls.push_back(call);
        }
    }
    return calls;
}

bool Client::is_terminal(const Response &response) { return response.stop_reason == "end_turn" || response.stop_reason == "stop_sequence"; }

bool Client::requires_tool_results(const Response &response) { return response.stop_reason == "tool_use"; }

void Client::append_response(History &history, Response response) {
    history.push_back({.role = std::string(k_role_assistant), .content = std::move(response.content)});
}

void Client::append_tool_results(History &history, std::vector<provider::ToolResult> results) {
    Message message{.role = std::string(k_role_user)};
    message.content.reserve(results.size());
    for (auto &result : results) {
        message.content.push_back(ToolResultBlock{
            .tool_use_id = std::move(result.call_id),
            .content = std::move(result.content),
            .is_error = result.is_error,
        });
    }
    history.push_back(std::move(message));
}

} // namespace liminal::anthropic
