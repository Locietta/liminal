#pragma once

#include <array>
#include <chrono>
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
#include "liminal/provider/common.h"

namespace liminal::openai {

using namespace lighter::types;

struct InputText {
    std::string text;
};

struct OutputText {
    std::string text;
};

struct Refusal {
    std::string refusal;
};

using MessageContent = std::variant<InputText, OutputText, Refusal>;

struct MessageItem {
    std::optional<std::string> id;
    std::string role;
    std::vector<MessageContent> content;
    std::optional<std::string> status;
};

struct FunctionCallItem {
    std::optional<std::string> id;
    std::string call_id;
    std::string name;
    std::string arguments;
    std::optional<std::string> status;
};

struct FunctionCallOutputItem {
    std::optional<std::string> id;
    std::string call_id;
    std::string output;
    std::optional<std::string> status;
};

struct ReasoningSummaryText {
    std::string type = "summary_text";
    std::string text;
};

struct ReasoningText {
    std::string type = "reasoning_text";
    std::string text;
};

struct ReasoningItem {
    std::string id;
    std::vector<ReasoningSummaryText> summary;
    std::vector<ReasoningText> content;
    std::optional<std::string> encrypted_content;
    std::optional<std::string> status;
};

struct CompactionItem {
    std::string id;
    std::string encrypted_content;
    std::optional<std::string> created_by;
};

using ResponseItem = std::variant<MessageItem, FunctionCallItem, FunctionCallOutputItem, ReasoningItem, CompactionItem>;

struct FunctionTool {
    std::string name;
    std::string description;
    provider::InputSchema parameters;
    bool strict = true;
};

using Tool = std::variant<FunctionTool>;

struct ResponseRequest {
    std::string model;
    u32 max_output_tokens = 8192;
    std::vector<ResponseItem> input;
    std::optional<std::vector<Tool>> tools;
    bool parallel_tool_calls = true;
    bool stream = true;
    bool store = false;
    std::vector<std::string> include = {"reasoning.encrypted_content"};
};

struct Usage {
    u64 input_tokens = 0;
    u64 output_tokens = 0;
    u64 total_tokens = 0;
    u64 cached_tokens = 0;
    u64 reasoning_tokens = 0;
};

struct Response {
    std::string id;
    std::string model;
    std::vector<ResponseItem> output;
    std::vector<provider::ToolCall> calls;
    Usage usage;
    std::string request_id;
    bool completed = false;
    bool emitted_text = false;
};

struct CompactRequest {
    std::string model;
    std::vector<ResponseItem> input;
    std::string instructions;
};

struct CompactResponse {
    std::vector<ResponseItem> output;
    std::string request_id;
};

struct ClientOptions {
    std::string api_key;
    std::string organization;
    std::string project;
    std::string base_url = "https://api.openai.com/v1";
    usize max_retries = 2;
    std::chrono::milliseconds initial_retry_delay{500};
};

struct Client {
    using History = std::vector<ResponseItem>;
    using Response = openai::Response;

    explicit Client(ClientOptions options);

    /// Low-level Responses API operation.
    lighter::Task<Response, Error> create_response(const ResponseRequest &request, const provider::StreamCallbacks &callbacks = {});

    /// OpenAI-specific remote context compaction operation.
    lighter::Task<CompactResponse, Error> compact(const CompactRequest &request);
    lighter::Task<void, Error> compact_history(std::string model, History &history, std::string instructions);

    /// Static provider contract used by Agent.
    lighter::Task<Response, Error> create_message(std::string model, u32 max_tokens, const History &history,
                                                  const std::vector<provider::ToolDefinition> &tools,
                                                  const provider::StreamCallbacks &callbacks = {});
    static void append_user(History &history, std::string prompt);
    static std::vector<const provider::ToolCall *> tool_calls(const Response &response);
    static bool is_terminal(const Response &response);
    static bool requires_tool_results(const Response &response);
    static void append_response(History &history, Response response);
    static void append_tool_results(History &history, std::vector<provider::ToolResult> results);

    ClientOptions options;
    lighter::http::Client http_client;
};

} // namespace liminal::openai

template <>
struct glz::meta<liminal::openai::MessageContent> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"input_text", "output_text", "refusal"};
};

template <>
struct glz::meta<liminal::openai::ResponseItem> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"message", "function_call", "function_call_output", "reasoning", "compaction"};
};

template <>
struct glz::meta<liminal::openai::Tool> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"function"};
};
