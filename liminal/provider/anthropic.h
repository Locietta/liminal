#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/async/async.h>
#include <lighter/http/http.h>
#include <lighter/types.hpp>

#include "liminal/error.h"

namespace liminal::anthropic {

using namespace lighter::types;

// --- roles -------------------------------------------------------------

inline constexpr std::string_view k_role_user = "user";
inline constexpr std::string_view k_role_assistant = "assistant";

// --- content blocks ----------------------------------------------------

struct TextBlock {
    std::string text;
};

struct ToolUseBlock {
    std::string id;
    std::string name;
    glz::generic input;
};

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

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock, RedactedThinkingBlock>;

// --- messages ----------------------------------------------------------

struct Message {
    std::string role; // k_role_user / k_role_assistant
    std::vector<ContentBlock> content;
};

inline Message user_text(std::string text) { return {.role = std::string(k_role_user), .content = {TextBlock{.text = std::move(text)}}}; }

// --- tool definitions --------------------------------------------------

struct SchemaProperty {
    std::string type;
    std::string description;
};

struct InputSchema {
    std::string type = "object";
    std::map<std::string, SchemaProperty> properties;
    std::vector<std::string> required;
    bool additional_properties = false; // serialized as "additionalProperties"
};

struct ToolDefinition {
    std::string name;
    std::string description;
    InputSchema input_schema;
};

// --- request / response ------------------------------------------------

struct MessageRequest {
    std::string model;
    u32 max_tokens = 8192;
    std::vector<Message> messages;
    std::optional<std::vector<ToolDefinition>> tools; // omitted when nullopt
    bool stream = true;
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
    std::string stop_reason; // kept as string for forward compatibility
    std::optional<std::string> stop_sequence;
    Usage usage;
    std::string request_id; // request-id response header
};

// --- client ------------------------------------------------------------

struct StreamCallbacks {
    /// Called for each text_delta as it arrives; may be empty.
    std::function<void(std::string_view)> on_text_delta;
};

struct ClientOptions {
    /// Sent as `x-api-key` (direct Anthropic API convention).
    std::string api_key;
    /// Sent as `Authorization: Bearer` (proxy/gateway convention). Either
    /// this or api_key must be set; both may be.
    std::string auth_token;
    std::string base_url = "https://api.anthropic.com";
    usize max_retries = 2;
    std::chrono::milliseconds initial_retry_delay{500};
};

struct Client {
    explicit Client(ClientOptions options);

    /// Streams one Messages API call to completion, invoking callbacks live.
    /// Retries transport/429/5xx failures with backoff, but only while no
    /// text has been surfaced through the callbacks.
    lighter::Task<AssistantMessage, Error> create_message(const MessageRequest &request, const StreamCallbacks &callbacks = {});

    ClientOptions options;
    lighter::http::Client http_client;
};

} // namespace liminal::anthropic

// --- glaze metadata ----------------------------------------------------

template <>
struct glz::meta<liminal::anthropic::ContentBlock> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"text", "tool_use", "tool_result", "thinking", "redacted_thinking"};
};

template <>
struct glz::meta<liminal::anthropic::InputSchema> {
    using T = liminal::anthropic::InputSchema;
    static constexpr auto value =
        object("type", &T::type, "properties", &T::properties, "required", &T::required, "additionalProperties", &T::additional_properties);
};
