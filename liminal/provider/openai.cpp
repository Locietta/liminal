#include "openai.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
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
#include <liminal/provider/detail/completion_retry.h>
#include <liminal/provider/detail/openai_compaction.h>
#include <liminal/provider/detail/openai_protocol.h>
#include <liminal/provider/provider.h>

// OpenAI Responses API wire types. Internal: the public surface speaks the
// neutral transcript (provider/history.h); everything here is serialization
// detail behind complete()/compact().
namespace liminal::openai::wire {

struct InputText {
    std::string text;
};

struct OutputText {
    std::string text;
    std::optional<std::vector<glz::generic>> annotations;
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
    std::optional<std::string> phase;
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

struct WebSearchCallItem {
    std::string id;
    std::optional<std::string> status;
    std::optional<glz::generic> action;
};

using ResponseItem = std::variant<MessageItem, FunctionCallItem, FunctionCallOutputItem, ReasoningItem, CompactionItem, WebSearchCallItem>;

/// Non-strict like Codex CLI: strict mode requires every property in
/// `required` (optionals as nullable unions), which our tool schemas with
/// genuinely optional parameters deliberately do not satisfy.
struct FunctionTool {
    std::string name;
    std::string description;
    provider::InputSchema parameters;
    bool strict = false;
};

struct WebSearchTool {};

using Tool = std::variant<FunctionTool, WebSearchTool>;

struct Reasoning {
    std::string effort;
};

struct ResponseRequest {
    std::string model;
    std::optional<std::string> instructions;
    std::optional<u32> max_output_tokens;
    std::vector<ResponseItem> input;
    std::optional<std::vector<Tool>> tools;
    bool parallel_tool_calls = true;
    bool stream = true;
    bool store = false;
    std::vector<std::string> include = {"reasoning.encrypted_content"};
    std::optional<Reasoning> reasoning;
};

struct CompactRequest {
    std::string model;
    std::vector<ResponseItem> input;
    std::string instructions;
};

struct Model {
    std::string id;
    std::string slug;
    std::string display_name;
};

struct ModelsResponse {
    std::vector<Model> data;
    std::vector<Model> models;
};

struct UrlCitation {
    std::string type;
    std::string url;
    std::optional<std::string> title;
};

} // namespace liminal::openai::wire

template <>
struct glz::meta<liminal::openai::wire::MessageContent> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"input_text", "output_text", "refusal"};
};

template <>
struct glz::meta<liminal::openai::wire::ResponseItem> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids =
        std::array{"message", "function_call", "function_call_output", "reasoning", "compaction", "web_search_call"};
};

template <>
struct glz::meta<liminal::openai::wire::Tool> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"function", "web_search"};
};

namespace liminal::openai {

namespace http = lighter::http;
namespace json = lighter::codec::json;
using lighter::fail;
using lighter::or_fail;
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

std::optional<std::string> citation_markdown(const glz::generic &annotation) {
    auto encoded = json::to_string(annotation);
    if (!encoded) return std::nullopt;
    auto citation = parse_wire<wire::UrlCitation>(*encoded, "URL citation");
    if (!citation || citation->type != "url_citation" || citation->url.empty()) return std::nullopt;
    const auto label = citation->title && !citation->title->empty() ? *citation->title : citation->url;
    return "[" + label + "](" + citation->url + ")";
}

std::string cited_text(wire::OutputText text) {
    if (!text.annotations || text.annotations->empty()) return std::move(text.text);
    std::vector<std::string> citations;
    for (const auto &annotation : *text.annotations) {
        auto citation = citation_markdown(annotation);
        if (citation && std::ranges::find(citations, *citation) == citations.end()) citations.push_back(*std::move(citation));
    }
    if (!citations.empty()) {
        text.text += "\n\nSources:";
        for (const auto &citation : citations) text.text += "\n- " + citation;
    }
    return std::move(text.text);
}

/// OpenAI error codes/types that indicate a transient upstream condition.
bool transient_api_type(std::string_view type) {
    return type == "rate_limit_error" || type == "rate_limit_exceeded" || type == "server_error" || type == "api_error" ||
           type == "overloaded_error" || type == "timeout_error";
}

// --- neutral <-> wire translation ---------------------------------------

Result<std::string> encode_opaque(const wire::ResponseItem &item) {
    auto encoded = json::to_string(item);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "opaque item"));
    }
    return *std::move(encoded);
}

Result<wire::FunctionCallItem> to_function_call(const provider::ToolCall &call) {
    auto arguments = json::to_string(call.input);
    if (!arguments) {
        return outcome_error(Error::json(std::move(arguments).error(), "tool call arguments"));
    }
    return wire::FunctionCallItem{.call_id = call.id, .name = call.name, .arguments = *std::move(arguments)};
}

std::string_view role_name(provider::Role role) {
    switch (role) {
        case provider::Role::SYSTEM: return "system";
        case provider::Role::DEVELOPER: return "developer";
        case provider::Role::USER: return "user";
        case provider::Role::ASSISTANT: return "assistant";
    }
    std::unreachable();
}

struct WireInput {
    std::vector<wire::ResponseItem> items;
    std::optional<std::string> instructions;
};

/// History -> Responses input items. One neutral item may fan out into
/// several wire items: contiguous text parts form one message; every tool
/// call/result and replayed opaque is its own top-level item. Opaque parts
/// carrying our tag (reasoning with encrypted content, compaction items) are
/// replayed verbatim; foreign-tagged parts are dropped. System instructions
/// are lifted into the top-level `instructions` field: the Codex backend
/// rejects system-role input items, and the standard Responses API treats the
/// field as the same authority tier.
Result<WireInput> to_wire(const provider::History &history) {
    WireInput result;
    auto &items = result.items;
    items.reserve(history.size());
    bool conversation_started = false;

    for (const auto &item : history) {
        const bool instruction = provider::is_instruction(item.role);
        if (instruction && conversation_started) {
            return outcome_error(Error::protocol("system and developer instructions must precede conversation messages"));
        }
        conversation_started = conversation_started || !instruction;
        if (item.role == provider::Role::SYSTEM) {
            for (const auto &part : item.parts) {
                const auto *text = std::get_if<provider::TextPart>(&part);
                if (!text) return outcome_error(Error::protocol("instruction messages may contain only text"));
                if (!result.instructions) result.instructions.emplace();
                if (!result.instructions->empty()) *result.instructions += "\n\n";
                *result.instructions += text->text;
            }
            continue;
        }
        const bool assistant = item.role == provider::Role::ASSISTANT;
        const auto role = std::string(role_name(item.role));
        wire::MessageItem message{.role = role};
        if (item.phase == provider::MessagePhase::COMMENTARY) message.phase = "commentary";
        if (item.phase == provider::MessagePhase::FINAL) message.phase = "final_answer";

        auto flush_message = [&] {
            if (!message.content.empty()) {
                auto next = wire::MessageItem{.role = role};
                next.phase = message.phase;
                items.push_back(std::exchange(message, std::move(next)));
            }
        };

        for (const auto &part : item.parts) {
            if (const auto *text = std::get_if<provider::TextPart>(&part)) {
                if (assistant) {
                    message.content.push_back(wire::OutputText{.text = text->text});
                } else {
                    message.content.push_back(wire::InputText{.text = text->text});
                }
            } else if (const auto *call = std::get_if<provider::ToolCall>(&part)) {
                if (instruction) return outcome_error(Error::protocol("instruction messages may contain only text"));
                flush_message();
                auto function = to_function_call(*call);
                if (!function) {
                    return outcome_error(std::move(function).error());
                }
                items.push_back(*std::move(function));
            } else if (const auto *result = std::get_if<provider::ToolResult>(&part)) {
                if (instruction) return outcome_error(Error::protocol("instruction messages may contain only text"));
                flush_message();
                items.push_back(wire::FunctionCallOutputItem{.call_id = result->call_id, .output = result->content});
            } else if (const auto *opaque = std::get_if<provider::OpaquePart>(&part)) {
                if (instruction) return outcome_error(Error::protocol("instruction messages may contain only text"));
                if (opaque->provider_tag != k_provider_tag) {
                    continue; // another provider's private state
                }
                flush_message();
                auto replayed = parse_wire<wire::ResponseItem>(opaque->payload, "opaque replay");
                if (!replayed) {
                    return outcome_error(std::move(replayed).error());
                }
                items.push_back(*std::move(replayed));
            }
        }
        flush_message();
    }
    return result;
}

provider::MessagePhase message_phase(const wire::MessageItem &message) {
    if (message.phase == "commentary") return provider::MessagePhase::COMMENTARY;
    if (message.phase == "final_answer") return provider::MessagePhase::FINAL;
    return provider::MessagePhase::UNSPECIFIED;
}

std::string item_id(const wire::ResponseItem &item, u64 output_index) {
    return std::visit(
        [output_index](const auto &value) {
            if constexpr (requires { value.id; }) {
                using Id = std::decay_t<decltype(value.id)>;
                if constexpr (std::is_same_v<Id, std::optional<std::string>>) {
                    if (value.id) return *value.id;
                } else if (!value.id.empty()) {
                    return value.id;
                }
            }
            return "output:" + std::to_string(output_index);
        },
        item);
}

provider::OutputItemHeader item_header(const wire::ResponseItem &item, u64 output_index) {
    provider::OutputItemHeader header{.id = {.value = item_id(item, output_index)}};
    if (const auto *message = std::get_if<wire::MessageItem>(&item)) {
        header.kind = provider::OutputItemKind::ASSISTANT_MESSAGE;
        header.phase = message_phase(*message);
    } else if (std::holds_alternative<wire::FunctionCallItem>(item)) {
        header.kind = provider::OutputItemKind::TOOL_CALL;
    }
    return header;
}

Result<provider::OutputItem> to_output_item(wire::ResponseItem item, u64 output_index, bool &refused) {
    const provider::OutputItemId id{.value = item_id(item, output_index)};
    if (auto *message = std::get_if<wire::MessageItem>(&item)) {
        provider::AssistantMessageItem output{.id = id, .phase = message_phase(*message)};
        for (auto &content : message->content) {
            if (auto *text = std::get_if<wire::OutputText>(&content)) {
                output.parts.push_back({.text = cited_text(std::move(*text))});
            } else if (auto *refusal = std::get_if<wire::Refusal>(&content)) {
                refused = true;
                output.parts.push_back({.text = std::move(refusal->refusal)});
            } else if (auto *input = std::get_if<wire::InputText>(&content)) {
                output.parts.push_back({.text = std::move(input->text)});
            }
        }
        return provider::OutputItem(std::move(output));
    }
    if (auto *function = std::get_if<wire::FunctionCallItem>(&item)) {
        auto input =
            parse_wire<glz::generic>(function->arguments.empty() ? std::string_view("{}") : function->arguments, "function_call arguments");
        if (!input) return outcome_error(std::move(input).error());
        if (!input->is_object()) return outcome_error(Error::protocol("function_call arguments are not a JSON object"));
        return provider::OutputItem(provider::ToolCallItem{
            .id = id,
            .call = {.id = std::move(function->call_id), .name = std::move(function->name), .input = *std::move(input)},
        });
    }
    if (std::holds_alternative<wire::ReasoningItem>(item) || std::holds_alternative<wire::CompactionItem>(item) ||
        std::holds_alternative<wire::WebSearchCallItem>(item)) {
        auto payload = encode_opaque(item);
        if (!payload) return outcome_error(std::move(payload).error());
        return provider::OutputItem(provider::ProviderOpaqueItem{
            .id = id,
            .part = {.provider_tag = std::string(k_provider_tag), .payload = *std::move(payload)},
        });
    }
    return outcome_error(Error::protocol("unexpected function_call_output in model output"));
}

void set_item_id(provider::OutputItem &item, provider::OutputItemId id) {
    std::visit([&id](auto &value) { value.id = std::move(id); }, item);
}

// --- SSE wire event payloads --------------------------------------------

struct WireType {
    std::string type;
};

struct OutputItemDoneEvent {
    u64 output_index = 0;
    glz::generic item;
};

using OutputItemAddedEvent = OutputItemDoneEvent;

struct TextDeltaEvent {
    std::string delta;
    std::optional<std::string> item_id;
    std::optional<u64> output_index;
};

struct AnnotationAddedEvent {
    glz::generic annotation;
    std::optional<std::string> item_id;
    std::optional<u64> output_index;
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
        std::optional<WireResponse::IncompleteDetails> incomplete_details;
    } response;
};

Result<wire::ResponseItem> parse_item(const glz::generic &value) {
    auto encoded = json::to_string(value);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "response output item"));
    }
    return parse_wire<wire::ResponseItem>(*encoded, "response output item");
}

// --- stream accumulator -------------------------------------------------

struct StreamAccumulator {
    provider::ProviderCallCompletion completion;
    std::vector<std::optional<wire::ResponseItem>> completed;
    std::vector<bool> started;
    std::vector<std::optional<provider::OutputItemId>> item_ids;
    std::string status;
    bool saw_created = false;
    bool saw_completed = false;
    bool output_emitted = false;
    bool has_calls = false;
    bool refused = false;

    provider::OutputItemId canonical_item_id(u64 output_index, provider::OutputItemId candidate) {
        const auto index = static_cast<usize>(output_index);
        if (item_ids.size() <= index) item_ids.resize(index + 1);
        if (!item_ids[index]) item_ids[index] = std::move(candidate);
        return *item_ids[index];
    }

    void apply_usage(const WireUsage &usage) {
        if (usage.input_tokens) completion.usage.input_tokens = *usage.input_tokens;
        if (usage.output_tokens) completion.usage.output_tokens = *usage.output_tokens;
        completion.usage.context_tokens =
            usage.total_tokens.value_or(completion.usage.input_tokens > std::numeric_limits<u64>::max() - completion.usage.output_tokens ?
                                            std::numeric_limits<u64>::max() :
                                            completion.usage.input_tokens + completion.usage.output_tokens);
        if (usage.input_tokens_details && usage.input_tokens_details->cached_tokens) {
            completion.usage.cache_read_tokens = *usage.input_tokens_details->cached_tokens;
        }
        if (usage.output_tokens_details && usage.output_tokens_details->reasoning_tokens) {
            completion.usage.reasoning_tokens = *usage.output_tokens_details->reasoning_tokens;
        }
    }

    Result<void> start_item(u64 output_index, const wire::ResponseItem &item, const provider::StreamCallbacks &callbacks) {
        const auto index = static_cast<usize>(output_index);
        if (started.size() <= index) started.resize(index + 1);
        auto header = item_header(item, output_index);
        header.id = canonical_item_id(output_index, std::move(header.id));
        if (started[index]) return {};
        started[index] = true;
        output_emitted = true;
        if (callbacks.on_item_started) callbacks.on_item_started(header);
        return {};
    }

    provider::OutputItemId start_assistant_item(u64 output_index, std::string id, const provider::StreamCallbacks &callbacks) {
        const auto index = static_cast<usize>(output_index);
        if (started.size() <= index) started.resize(index + 1);
        auto canonical = canonical_item_id(output_index, {.value = std::move(id)});
        if (started[index]) return canonical;
        started[index] = true;
        output_emitted = true;
        if (callbacks.on_item_started) {
            callbacks.on_item_started({
                .id = canonical,
                .kind = provider::OutputItemKind::ASSISTANT_MESSAGE,
            });
        }
        return canonical;
    }

    Result<void> add_item(u64 output_index, wire::ResponseItem item, const provider::StreamCallbacks &callbacks) {
        auto index = static_cast<usize>(output_index);
        if (completed.size() <= index) {
            completed.resize(index + 1);
        }
        if (completed[index]) {
            return outcome_error(Error::protocol("duplicate response.output_item.done index"));
        }
        auto started_result = start_item(output_index, item, callbacks);
        if (!started_result) return started_result;
        has_calls = has_calls || std::holds_alternative<wire::FunctionCallItem>(item);
        auto output = to_output_item(item, output_index, refused);
        if (!output) return outcome_error(std::move(output).error());
        set_item_id(*output, canonical_item_id(output_index, item_header(item, output_index).id));
        completed[index] = std::move(item);
        if (callbacks.on_item_completed) callbacks.on_item_completed(*output);
        return {};
    }

    Result<void> add_response_output(const WireResponse &response, const provider::StreamCallbacks &callbacks) {
        if (!response.output) return {};
        for (usize index = 0; index < response.output->size(); ++index) {
            if (index < completed.size() && completed[index]) continue;
            auto item = parse_item((*response.output)[index]);
            if (!item) return outcome_error(std::move(item).error());
            auto added = add_item(index, *std::move(item), callbacks);
            if (!added) return added;
        }
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
            completion.model = std::move(parsed->response.model);
            return {};
        }
        if (!saw_created) {
            return outcome_error(Error::protocol("event before response.created: " + name));
        }
        if (name == "response.output_item.added") {
            auto parsed = parse_wire<OutputItemAddedEvent>(event.data, name);
            if (!parsed) return outcome_error(std::move(parsed).error());
            auto item = parse_item(parsed->item);
            if (!item) return outcome_error(std::move(item).error());
            return start_item(parsed->output_index, *item, callbacks);
        }
        if (name == "response.output_text.delta" || name == "response.refusal.delta") {
            auto parsed = parse_wire<TextDeltaEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            output_emitted = true;
            const auto output_index = parsed->output_index.value_or(static_cast<u64>(completed.size()));
            auto id = parsed->item_id.value_or("output:" + std::to_string(output_index));
            auto canonical = start_assistant_item(output_index, std::move(id), callbacks);
            if (callbacks.on_assistant_text_delta && !parsed->delta.empty()) {
                callbacks.on_assistant_text_delta(canonical, parsed->delta);
            }
            return {};
        }
        if (name == "response.output_text.annotation.added") {
            auto parsed = parse_wire<AnnotationAddedEvent>(event.data, name);
            if (!parsed) return outcome_error(std::move(parsed).error());
            auto citation = citation_markdown(parsed->annotation);
            const auto output_index = parsed->output_index.value_or(static_cast<u64>(completed.size()));
            auto id = parsed->item_id.value_or("output:" + std::to_string(output_index));
            auto canonical = start_assistant_item(output_index, std::move(id), callbacks);
            if (citation && callbacks.on_assistant_text_delta) {
                output_emitted = true;
                callbacks.on_assistant_text_delta(canonical, "\n\nSource: " + *citation);
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
            return add_item(parsed->output_index, *std::move(item), callbacks);
        }
        if (name == "response.completed") {
            auto parsed = parse_wire<ResponseEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            if (parsed->response.status != "completed") {
                return outcome_error(Error::protocol("response.completed carried status: " + parsed->response.status));
            }
            if (completion.model.empty()) completion.model = std::move(parsed->response.model);
            if (parsed->response.usage) apply_usage(*parsed->response.usage);

            // Non-streaming-shaped gateways may carry some or all output only
            // on the terminal response object.
            auto added = add_response_output(parsed->response, callbacks);
            if (!added) return added;
            status = "completed";
            saw_completed = true;
            return {};
        }
        if (name == "response.incomplete") {
            auto parsed = parse_wire<ResponseEvent>(event.data, name);
            if (!parsed) return outcome_error(std::move(parsed).error());
            if (completion.model.empty()) completion.model = std::move(parsed->response.model);
            if (parsed->response.usage) apply_usage(*parsed->response.usage);
            auto added = add_response_output(parsed->response, callbacks);
            if (!added) return added;
            const auto reason = parsed->response.incomplete_details ? parsed->response.incomplete_details->reason : "unknown";
            completion.stop = reason == "max_output_tokens" || reason == "max_tokens" ? provider::StopKind::TRUNCATED :
                              reason == "context_length_exceeded" || reason == "context_window_exceeded" ?
                                                                                        provider::StopKind::CONTEXT_EXHAUSTED :
                              reason == "content_filter" ? provider::StopKind::REFUSED :
                                                           provider::StopKind::OTHER;
            completion.stop_detail = reason;
            status = "incomplete";
            saw_completed = true;
            return {};
        }
        if (name == "response.failed") {
            auto parsed = parse_wire<FailedResponseEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            if (parsed->response.error) {
                auto type = parsed->response.error->code.value_or(parsed->response.error->type.value_or(""));
                bool transient = transient_api_type(type);
                return outcome_error(
                    Error::api(std::move(type), std::move(parsed->response.error->message), completion.request_id, transient));
            }
            return outcome_error(Error::protocol(name + " without error detail"));
        }
        if (name == "error") {
            auto parsed = parse_wire<ErrorEvent>(event.data, name);
            if (!parsed) {
                return outcome_error(std::move(parsed).error());
            }
            auto code = parsed->code.value_or("");
            bool transient = transient_api_type(code);
            return outcome_error(Error::api(std::move(code), std::move(parsed->message), completion.request_id, transient));
        }
        return {};
    }

    Result<provider::ProviderCallCompletion> finish() && {
        if (!saw_completed) {
            return outcome_error(Error::protocol("stream ended before response.completed"));
        }
        for (auto &entry : completed) {
            if (!entry) {
                return outcome_error(Error::protocol("response output item index gap"));
            }
        }
        if (status == "completed") {
            completion.stop = refused   ? provider::StopKind::REFUSED :
                              has_calls ? provider::StopKind::NEEDS_TOOL_RESULTS :
                                          provider::StopKind::DONE;
            completion.stop_detail = std::move(status);
        }
        return std::move(completion);
    }
};

// --- request helpers ----------------------------------------------------

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
void apply_auth_headers(RequestType &request, const provider::ResolvedAuth &auth) {
    if (!auth.bearer_token.empty()) {
        request.bearer_auth(auth.bearer_token);
    }
    if (!auth.api_key.empty()) {
        request.header("x-api-key", auth.api_key);
    }
    for (const auto &header : auth.headers) {
        request.header(header.name, header.value);
    }
}

Task<provider::ResolvedAuth, Error> resolve_auth(const ClientOptions &options) {
    if (!options.auth) {
        co_await fail(Error::config("provider has no authentication resolver"));
    }
    co_return co_await options.auth().or_fail();
}

/// The Codex backend reports request validation failures as a bare
/// `{"detail": "..."}` object instead of the standard error envelope.
struct DetailErrorEnvelope {
    std::optional<std::string> detail;
};

Error parse_status_error(int status, std::string_view body, std::string request_id, std::optional<std::chrono::milliseconds> retry_after) {
    std::string api_type;
    std::string detail(body);
    if (auto envelope = parse_wire<ApiErrorEnvelope>(body, "error body"); envelope && !envelope->error.message.empty()) {
        api_type = envelope->error.code.value_or(envelope->error.type.value_or(""));
        detail = std::move(envelope->error.message);
    } else if (auto bare = parse_wire<DetailErrorEnvelope>(body, "error body"); bare && bare->detail && !bare->detail->empty()) {
        detail = *std::move(bare->detail);
    }
    return Error::http_status(status, std::move(api_type), std::move(detail), std::move(request_id), retry_after);
}

Task<provider::ProviderCallCompletion, Error> attempt_stream(http::Client &http_client, const ClientOptions &options,
                                                             const std::string &body, const provider::StreamCallbacks &callbacks,
                                                             bool &output_emitted) {
    auto request = http_client.on().post(options.base_url + "/responses");
    request.header("accept", "text/event-stream").json_text(body);
    apply_auth_headers(request, co_await resolve_auth(options).or_fail());

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
    const bool missing_allowed = options.allow_missing_event_stream_content_type && content_type.empty();
    if (!missing_allowed && content_type.find("text/event-stream") == std::string_view::npos) {
        co_await fail(Error::protocol("expected text/event-stream response, got: " + std::string(content_type)));
    }

    http::sse::EventStream events(std::move(response));
    StreamAccumulator accumulator;
    accumulator.completion.request_id = std::move(request_id);
    while (!accumulator.saw_completed) {
        auto event = co_await events.next();
        if (!event) {
            output_emitted = accumulator.output_emitted;
            co_await fail(Error::http(std::move(event).error()));
        }
        if (!*event) {
            break;
        }
        auto consumed = accumulator.consume(**event, callbacks);
        output_emitted = accumulator.output_emitted;
        if (!consumed) {
            co_await fail(std::move(consumed).error());
        }
    }
    co_return co_await or_fail(std::move(accumulator).finish());
}

std::optional<std::vector<wire::Tool>> make_tools(const std::vector<provider::ToolDefinition> &definitions) {
    if (definitions.empty()) {
        return std::nullopt;
    }
    std::vector<wire::Tool> tools;
    tools.reserve(definitions.size());
    for (const auto &definition : definitions) {
        switch (definition.kind) {
            case provider::ToolKind::FUNCTION:
                tools.push_back(wire::FunctionTool{
                    .name = definition.name,
                    .description = definition.description,
                    .parameters = definition.input_schema,
                });
                break;
            case provider::ToolKind::WEB_SEARCH: tools.push_back(wire::WebSearchTool{}); break;
            case provider::ToolKind::WEB_FETCH:
                // The Responses API's web_search tool performs both search
                // and page opening, so it has no separate fetch declaration.
                break;
        }
    }
    return tools.empty() ? std::nullopt : std::optional(std::move(tools));
}

// --- remote compaction ---------------------------------------------------

struct CompactEnvelope {
    std::vector<glz::generic> output;
};

/// One attempt against OpenAI's proprietary `POST /responses/compact`
/// (stateless: full input in, compacted items out). Modeled on codex's
/// remote-compaction path.
Task<std::vector<provider::OpaquePart>, Error> attempt_remote_compact(http::Client &http_client, const ClientOptions &options,
                                                                      const std::string &body) {
    auto request = http_client.on().post(options.base_url + "/responses/compact");
    request.json_text(body);
    apply_auth_headers(request, co_await resolve_auth(options).or_fail());

    auto sent = co_await std::move(request).send();
    if (!sent) {
        co_await fail(Error::http(std::move(sent).error()));
    }
    auto response = *std::move(sent);
    std::string request_id(response.header_value("x-request-id").value_or(""));
    if (!response.ok()) {
        co_await fail(parse_status_error(response.status, response.text(), std::move(request_id), parse_retry_after(response)));
    }

    auto envelope = parse_wire<CompactEnvelope>(response.text(), "compact response");
    if (!envelope) {
        co_await fail(std::move(envelope).error());
    }
    std::vector<provider::OpaquePart> output;
    output.reserve(envelope->output.size());
    for (const auto &value : envelope->output) {
        auto item = co_await or_fail(parse_item(value));
        auto payload = co_await or_fail(encode_opaque(item));
        output.push_back({
            .provider_tag = std::string(k_provider_tag),
            .payload = std::move(payload),
        });
    }
    co_return output;
}

} // namespace

namespace protocol {

Result<std::string> encode_complete_request(const provider::History &history, const std::vector<provider::ToolDefinition> &tools,
                                            const ClientOptions &options) {
    auto input = to_wire(history);
    if (!input) {
        return outcome_error(std::move(input).error());
    }
    wire::ResponseRequest request{
        .model = options.model,
        .instructions = std::move(input->instructions),
        .max_output_tokens = options.max_output_tokens,
        .input = std::move(input->items),
        .tools = make_tools(tools),
    };
    if (options.reasoning_effort) {
        request.reasoning = wire::Reasoning{.effort = *options.reasoning_effort};
    }

    auto encoded = json::to_string(request);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "response request body"));
    }
    return *std::move(encoded);
}

Result<std::string> encode_compact_request(const provider::History &conversation, std::string_view instructions,
                                           const ClientOptions &options) {
    auto input = to_wire(conversation);
    if (!input) {
        return outcome_error(std::move(input).error());
    }
    wire::CompactRequest request{
        .model = options.model,
        .input = std::move(input->items),
        .instructions = std::string(instructions),
    };
    auto encoded = json::to_string(request);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "compact request body"));
    }
    return *std::move(encoded);
}

Result<provider::ProviderCallCompletion> decode_stream(std::span<const http::sse::Event> events, const provider::StreamCallbacks &callbacks,
                                                       std::string request_id) {
    StreamAccumulator accumulator;
    accumulator.completion.request_id = std::move(request_id);
    for (const auto &event : events) {
        auto consumed = accumulator.consume(event, callbacks);
        if (!consumed) {
            return outcome_error(std::move(consumed).error());
        }
    }
    return std::move(accumulator).finish();
}

} // namespace protocol

Client::Client(ClientOptions options) : options(std::move(options)) {
    while (!this->options.base_url.empty() && this->options.base_url.back() == '/') {
        this->options.base_url.pop_back();
    }
}

Task<provider::ProviderCallCompletion, Error> Client::complete(const provider::History &history,
                                                               const std::vector<provider::ToolDefinition> &tools,
                                                               const provider::StreamCallbacks &callbacks) {
    const std::string body = co_await or_fail(protocol::encode_complete_request(history, tools, options));
    provider::detail::CompletionAttempts attempts{
        .stream = [this](const std::string &request_body, const provider::StreamCallbacks &stream_callbacks,
                         bool &output_emitted) -> Task<provider::ProviderCallCompletion, Error> {
            return attempt_stream(http_client, options, request_body, stream_callbacks, output_emitted);
        },
        .sleep = [](std::chrono::milliseconds delay) { return lighter::sleep(delay); },
    };
    co_return co_await provider::detail::complete_with_retry(attempts, body, callbacks, options.max_retries, options.initial_retry_delay)
        .or_fail();
}

Task<void, Error> Client::compact(provider::History &history, std::string_view instructions) {
    const auto instruction_count = provider::instruction_prefix_size(history);
    if (instruction_count == history.size()) co_return;
    provider::History conversation(history.begin() + instruction_count, history.end());
    const std::string body = co_await or_fail(protocol::encode_compact_request(conversation, instructions, options));
    detail::CompactionAttempts attempts{
        .remote = [this](const std::string &request_body) { return attempt_remote_compact(http_client, options, request_body); },
        .local = [this](provider::History &target,
                        std::string_view compact_instructions) { return provider::local_compact(this, target, compact_instructions); },
        .sleep = [](std::chrono::milliseconds delay) { return lighter::sleep(delay); },
    };
    co_return co_await detail::compact_with_retry(attempts, history, instruction_count, body, std::string(instructions),
                                                  options.max_retries, options.initial_retry_delay)
        .or_fail();
}

namespace {

Task<std::vector<provider::DiscoveredModel>, Error> list_models_impl(ClientOptions options, std::string client_version) {
    while (!options.base_url.empty() && options.base_url.back() == '/') {
        options.base_url.pop_back();
    }

    http::Client client;
    auto request = client.on().get(options.base_url + "/models");
    if (!client_version.empty()) {
        request.query("client_version", client_version);
    }
    apply_auth_headers(request, co_await resolve_auth(options).or_fail());
    auto sent = co_await std::move(request).send();
    if (!sent) {
        co_await fail(Error::http(std::move(sent).error()));
    }
    if (!sent->ok()) {
        auto request_id = std::string(sent->header_value("x-request-id").value_or(""));
        co_await fail(parse_status_error(sent->status, sent->text(), std::move(request_id), {}));
    }

    auto response = parse_wire<wire::ModelsResponse>(sent->text(), "models response");
    if (!response) {
        co_await fail(std::move(response).error());
    }
    std::vector<provider::DiscoveredModel> models;
    models.reserve(response->data.size() + response->models.size());
    for (auto &entry : response->data) {
        if (!entry.id.empty()) {
            models.push_back({.id = std::move(entry.id), .name = {}});
        }
    }
    for (auto &entry : response->models) {
        if (!entry.slug.empty()) {
            models.push_back({.id = std::move(entry.slug), .name = std::move(entry.display_name)});
        }
    }
    co_return models;
}

} // namespace

Task<std::vector<provider::DiscoveredModel>, Error> list_models(ClientOptions options) {
    co_return co_await list_models_impl(std::move(options), {}).or_fail();
}

Task<std::vector<provider::DiscoveredModel>, Error> list_codex_models(ClientOptions options, std::string compatibility_version) {
    co_return co_await list_models_impl(std::move(options), std::move(compatibility_version)).or_fail();
}

} // namespace liminal::openai
