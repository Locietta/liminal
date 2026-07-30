#include "openai.h"

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
#include <lighter/codec/json/json.h>
#include <lighter/http/http.h>

namespace liminal::openai {

namespace http = lighter::http;
namespace json = lighter::codec::json;
using lighter::fail;
using lighter::outcome_error;
using lighter::Task;

namespace {

inline constexpr json::Opts k_wire_opts{{.null_terminated = false, .error_on_unknown_keys = false}};

template <typename T>
Result<T> parse_wire(std::string_view text, std::string_view context) {
    T value{};
    if (auto ctx = glz::read<k_wire_opts>(value, text)) {
        return outcome_error(Error::json({.code = ctx.ec, .detail = glz::format_error(ctx, text)}, std::string(context)));
    }
    return value;
}

struct WireType {
    std::string type;
};

struct OutputItemDoneEvent {
    u64 output_index = 0;
    glz::generic item;
};

struct TextDeltaEvent {
    std::string delta;
};

struct WireInputTokenDetails {
    std::optional<u64> cached_tokens;
};

struct WireOutputTokenDetails {
    std::optional<u64> reasoning_tokens;
};

struct WireUsage {
    std::optional<u64> input_tokens;
    std::optional<u64> output_tokens;
    std::optional<u64> total_tokens;
    std::optional<WireInputTokenDetails> input_tokens_details;
    std::optional<WireOutputTokenDetails> output_tokens_details;
};

struct WireResponse {
    struct IncompleteDetails {
        std::string reason;
    };

    std::string id;
    std::string model;
    std::string status;
    std::optional<IncompleteDetails> incomplete_details;
    std::optional<WireUsage> usage;
    std::optional<std::vector<glz::generic>> output;
};

struct ResponseEvent {
    WireResponse response;
};

struct WireApiError {
    std::string message;
    std::optional<std::string> type;
    std::optional<std::string> code;
};

struct ApiErrorEnvelope {
    WireApiError error;
};

struct ErrorEvent {
    std::optional<std::string> code;
    std::string message;
};

struct FailedResponseEvent {
    struct FailedResponse {
        std::optional<WireApiError> error;
    } response;
};

Result<ResponseItem> parse_item(const glz::generic &value) {
    auto encoded = json::to_string(value);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "response output item"));
    }
    auto type = parse_wire<WireType>(*encoded, "response output item type");
    if (!type) {
        return outcome_error(std::move(type).error());
    }

    if (type->type == "message") {
        auto item = parse_wire<MessageItem>(*encoded, "message output item");
        if (!item) {
            return outcome_error(std::move(item).error());
        }
        return ResponseItem(*std::move(item));
    }
    if (type->type == "function_call") {
        auto item = parse_wire<FunctionCallItem>(*encoded, "function_call output item");
        if (!item) {
            return outcome_error(std::move(item).error());
        }
        return ResponseItem(*std::move(item));
    }
    if (type->type == "function_call_output") {
        auto item = parse_wire<FunctionCallOutputItem>(*encoded, "function_call_output item");
        if (!item) {
            return outcome_error(std::move(item).error());
        }
        return ResponseItem(*std::move(item));
    }
    if (type->type == "reasoning") {
        auto item = parse_wire<ReasoningItem>(*encoded, "reasoning output item");
        if (!item) {
            return outcome_error(std::move(item).error());
        }
        return ResponseItem(*std::move(item));
    }
    if (type->type == "compaction") {
        auto item = parse_wire<CompactionItem>(*encoded, "compaction output item");
        if (!item) {
            return outcome_error(std::move(item).error());
        }
        return ResponseItem(*std::move(item));
    }
    return outcome_error(Error::protocol("unknown OpenAI response item type: " + type->type));
}

Result<provider::ToolCall> make_tool_call(const FunctionCallItem &item) {
    auto input = parse_wire<glz::generic>(item.arguments, "function_call arguments");
    if (!input) {
        return outcome_error(std::move(input).error());
    }
    if (!input->is_object()) {
        return outcome_error(Error::protocol("function_call arguments are not a JSON object"));
    }
    return provider::ToolCall{
        .id = item.call_id,
        .name = item.name,
        .input = *std::move(input),
    };
}

struct StreamAccumulator {
    Response response;
    std::vector<std::optional<ResponseItem>> completed;
    bool saw_created = false;
    bool saw_completed = false;

    void apply_usage(const WireUsage &usage) {
        if (usage.input_tokens) response.usage.input_tokens = *usage.input_tokens;
        if (usage.output_tokens) response.usage.output_tokens = *usage.output_tokens;
        if (usage.total_tokens) response.usage.total_tokens = *usage.total_tokens;
        if (usage.input_tokens_details && usage.input_tokens_details->cached_tokens) {
            response.usage.cached_tokens = *usage.input_tokens_details->cached_tokens;
        }
        if (usage.output_tokens_details && usage.output_tokens_details->reasoning_tokens) {
            response.usage.reasoning_tokens = *usage.output_tokens_details->reasoning_tokens;
        }
    }

    Result<void> add_item(u64 output_index, ResponseItem item) {
        auto index = static_cast<usize>(output_index);
        if (completed.size() <= index) {
            completed.resize(index + 1);
        }
        if (completed[index]) {
            return outcome_error(Error::protocol("duplicate response.output_item.done index"));
        }
        completed[index] = std::move(item);
        return {};
    }

    Result<void> consume(const http::sse::Event &event, const provider::StreamCallbacks &callbacks) {
        const auto &name = event.event;
        if (event.data == "[DONE]") {
            return {};
        }
        if (name == "response.created") {
            if (saw_created) {
                return outcome_error(Error::protocol("duplicate response.created"));
            }
            auto parsed = parse_wire<ResponseEvent>(event.data, "response.created");
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            saw_created = true;
            response.id = std::move(parsed->response.id);
            response.model = std::move(parsed->response.model);
            return {};
        }
        if (!saw_created) {
            return outcome_error(Error::protocol("event before response.created: " + name));
        }
        if (name == "response.output_text.delta" || name == "response.refusal.delta") {
            auto parsed = parse_wire<TextDeltaEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            response.emitted_text = true;
            if (callbacks.on_text_delta && !parsed->delta.empty()) {
                callbacks.on_text_delta(parsed->delta);
            }
            return {};
        }
        if (name == "response.output_item.done") {
            auto parsed = parse_wire<OutputItemDoneEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            auto item = parse_item(parsed->item);
            if (!item) {
                return outcome_error(std::move(item).error());
            }
            return add_item(parsed->output_index, *std::move(item));
        }
        if (name == "response.completed") {
            auto parsed = parse_wire<ResponseEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            if (parsed->response.status != "completed") {
                return outcome_error(Error::protocol("response.completed carried status: " + parsed->response.status));
            }
            if (response.id.empty()) response.id = std::move(parsed->response.id);
            if (response.model.empty()) response.model = std::move(parsed->response.model);
            if (parsed->response.usage) apply_usage(*parsed->response.usage);

            if (completed.empty() && parsed->response.output) {
                for (usize index = 0; index < parsed->response.output->size(); ++index) {
                    auto item = parse_item((*parsed->response.output)[index]);
                    if (!item) {
                        return outcome_error(std::move(item).error());
                    }
                    auto added = add_item(index, *std::move(item));
                    if (!added) {
                        return added;
                    }
                }
            }
            saw_completed = true;
            response.completed = true;
            return {};
        }
        if (name == "response.failed" || name == "response.incomplete") {
            auto parsed = parse_wire<FailedResponseEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            if (parsed->response.error) {
                auto type = parsed->response.error->code.value_or(parsed->response.error->type.value_or(""));
                return outcome_error(
                    Error::http_status(0, std::move(type), std::move(parsed->response.error->message), response.request_id));
            }
            if (name == "response.incomplete") {
                auto response = parse_wire<ResponseEvent>(event.data, name);
                if (response && response->response.incomplete_details) {
                    return outcome_error(Error::protocol("response incomplete: " + response->response.incomplete_details->reason));
                }
            }
            return outcome_error(Error::protocol(name + " without error detail"));
        }
        if (name == "error") {
            auto parsed = parse_wire<ErrorEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            return outcome_error(Error::http_status(0, parsed->code.value_or(""), std::move(parsed->message), response.request_id));
        }
        return {};
    }

    Result<Response> finish() && {
        if (!saw_completed) {
            return outcome_error(Error::protocol("OpenAI stream ended before response.completed"));
        }
        response.output.reserve(completed.size());
        for (auto &entry : completed) {
            if (!entry) {
                return outcome_error(Error::protocol("OpenAI output item index gap"));
            }
            response.output.push_back(*std::move(entry));
            if (const auto *function = std::get_if<FunctionCallItem>(&response.output.back())) {
                auto call = make_tool_call(*function);
                if (!call) {
                    return outcome_error(std::move(call).error());
                }
                response.calls.push_back(*std::move(call));
            }
        }
        return std::move(response);
    }
};

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

template <typename ResponseType>
std::optional<std::chrono::milliseconds> parse_retry_after(const ResponseType &response) {
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

template <typename RequestType>
void apply_auth_headers(RequestType &request, const ClientOptions &options) {
    request.bearer_auth(options.api_key);
    if (!options.organization.empty()) {
        request.header("OpenAI-Organization", options.organization);
    }
    if (!options.project.empty()) {
        request.header("OpenAI-Project", options.project);
    }
}

Error parse_status_error(int status, std::string_view body, std::string request_id, std::optional<std::chrono::milliseconds> retry_after) {
    std::string api_type;
    std::string detail(body);
    if (auto envelope = parse_wire<ApiErrorEnvelope>(body, "error body")) {
        api_type = envelope->error.code.value_or(envelope->error.type.value_or(""));
        detail = std::move(envelope->error.message);
    }
    return Error::http_status(status, std::move(api_type), std::move(detail), std::move(request_id), retry_after);
}

Task<Response, Error> attempt_stream(http::Client &http_client, const ClientOptions &options, const std::string &body,
                                     const provider::StreamCallbacks &callbacks, bool &text_emitted) {
    auto request = http_client.on().post(options.base_url + "/responses");
    request.header("accept", "text/event-stream").json_text(body);
    apply_auth_headers(request, options);

    auto streamed = co_await std::move(request).stream();
    if (!streamed) {
        co_await fail(Error::http(std::move(streamed).error()));
    }
    auto response = *std::move(streamed);
    std::string request_id(response.header_value("x-request-id").value_or(""));

    if (!response.ok()) {
        auto error_body = co_await read_bounded_body(response, 64 * 1024);
        co_await fail(parse_status_error(response.status, error_body, std::move(request_id), parse_retry_after(response)));
    }
    auto content_type = response.header_value("content-type").value_or("");
    if (content_type.find("text/event-stream") == std::string_view::npos) {
        co_await fail(Error::protocol("expected text/event-stream response, got: " + std::string(content_type)));
    }

    http::sse::EventStream events(std::move(response));
    StreamAccumulator accumulator;
    accumulator.response.request_id = std::move(request_id);
    while (!accumulator.saw_completed) {
        auto event = co_await events.next();
        if (!event) {
            text_emitted = accumulator.response.emitted_text;
            co_await fail(Error::http(std::move(event).error()));
        }
        if (!*event) {
            break;
        }
        auto consumed = accumulator.consume(**event, callbacks);
        text_emitted = accumulator.response.emitted_text;
        if (!consumed) {
            co_await fail(std::move(consumed).error());
        }
    }
    co_return co_await lighter::or_fail(std::move(accumulator).finish());
}

struct CompactEnvelope {
    std::vector<ResponseItem> output;
};

Task<CompactResponse, Error> attempt_compact(http::Client &http_client, const ClientOptions &options, const std::string &body) {
    auto request = http_client.on().post(options.base_url + "/responses/compact");
    request.json_text(body);
    apply_auth_headers(request, options);

    auto sent = co_await std::move(request).send();
    if (!sent) {
        co_await fail(Error::http(std::move(sent).error()));
    }
    auto response = *std::move(sent);
    std::string request_id(response.header_value("x-request-id").value_or(""));
    if (!response.ok()) {
        co_await fail(parse_status_error(response.status, response.text(), std::move(request_id), parse_retry_after(response)));
    }
    auto parsed = parse_wire<CompactEnvelope>(response.text(), "compact response");
    if (!parsed) {
        co_await fail(std::move(parsed).error());
    }
    co_return CompactResponse{.output = std::move(parsed->output), .request_id = std::move(request_id)};
}

std::vector<Tool> make_tools(const std::vector<provider::ToolDefinition> &definitions) {
    std::vector<Tool> tools;
    tools.reserve(definitions.size());
    for (const auto &definition : definitions) {
        tools.push_back(FunctionTool{
            .name = definition.name,
            .description = definition.description,
            .parameters = definition.input_schema,
        });
    }
    return tools;
}

} // namespace

Client::Client(ClientOptions options) : options(std::move(options)) {
    while (!this->options.base_url.empty() && this->options.base_url.back() == '/') {
        this->options.base_url.pop_back();
    }
}

Task<Response, Error> Client::create_response(const ResponseRequest &request, const provider::StreamCallbacks &callbacks) {
    auto encoded = json::to_string(request);
    if (!encoded) {
        co_await fail(Error::json(std::move(encoded).error(), "response request body"));
    }
    const std::string body = *std::move(encoded);

    bool text_emitted = false;
    for (usize attempt = 0;; ++attempt) {
        auto outcome = co_await attempt_stream(http_client, options, body, callbacks, text_emitted);
        if (outcome) {
            co_return *std::move(outcome);
        }
        auto error = std::move(outcome).error();
        if (attempt >= options.max_retries || !error.retryable() || text_emitted) {
            co_await fail(std::move(error));
        }
        auto delay = error.retry_after.value_or(options.initial_retry_delay * (1 << attempt));
        co_await lighter::sleep(delay);
    }
}

Task<CompactResponse, Error> Client::compact(const CompactRequest &request) {
    auto encoded = json::to_string(request);
    if (!encoded) {
        co_await fail(Error::json(std::move(encoded).error(), "compact request body"));
    }
    const std::string body = *std::move(encoded);

    for (usize attempt = 0;; ++attempt) {
        auto outcome = co_await attempt_compact(http_client, options, body);
        if (outcome) {
            co_return *std::move(outcome);
        }
        auto error = std::move(outcome).error();
        if (attempt >= options.max_retries || !error.retryable()) {
            co_await fail(std::move(error));
        }
        auto delay = error.retry_after.value_or(options.initial_retry_delay * (1 << attempt));
        co_await lighter::sleep(delay);
    }
}

Task<void, Error> Client::compact_history(std::string model, History &history, std::string instructions) {
    auto compacted = co_await compact({
        .model = std::move(model),
        .input = history,
        .instructions = std::move(instructions),
    });
    if (!compacted) {
        co_await fail(std::move(compacted).error());
    }
    history = std::move(compacted->output);
}

Task<Response, Error> Client::create_message(std::string model, u32 max_tokens, const History &history,
                                             const std::vector<provider::ToolDefinition> &tools,
                                             const provider::StreamCallbacks &callbacks) {
    co_return co_await create_response(
        {
            .model = std::move(model),
            .max_output_tokens = max_tokens,
            .input = history,
            .tools = make_tools(tools),
        },
        callbacks);
}

void Client::append_user(History &history, std::string prompt) {
    history.push_back(MessageItem{
        .role = "user",
        .content = {InputText{.text = std::move(prompt)}},
    });
}

std::vector<const provider::ToolCall *> Client::tool_calls(const Response &response) {
    std::vector<const provider::ToolCall *> calls;
    calls.reserve(response.calls.size());
    for (const auto &call : response.calls) {
        calls.push_back(&call);
    }
    return calls;
}

bool Client::is_terminal(const Response &response) { return response.completed && response.calls.empty(); }

bool Client::requires_tool_results(const Response &response) { return response.completed && !response.calls.empty(); }

void Client::append_response(History &history, Response response) {
    history.reserve(history.size() + response.output.size());
    for (auto &item : response.output) {
        history.push_back(std::move(item));
    }
}

void Client::append_tool_results(History &history, std::vector<provider::ToolResult> results) {
    history.reserve(history.size() + results.size());
    for (auto &result : results) {
        history.push_back(FunctionCallOutputItem{
            .call_id = std::move(result.call_id),
            .output = std::move(result.content),
        });
    }
}

} // namespace liminal::openai
