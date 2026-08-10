#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/http/sse.h>

#include <liminal/provider/detail/anthropic_protocol.h>

namespace {

using namespace liminal;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

glz::generic object(std::string_view json) {
    glz::generic value;
    auto error = glz::read_json(value, json);
    require(!error, "failed to build generic JSON object");
    return value;
}

provider::History base_history() {
    provider::History history;
    provider::append_system(history, "Test system policy.");
    provider::append_developer(history, "Test developer policy.");
    provider::append_user(history, "inspect");
    return history;
}

anthropic::ClientOptions options() {
    return {
        .model = "adaptive-model",
        .reasoning_effort = "high",
        .max_tokens = 4096,
    };
}

void test_request_encoding() {
    auto history = base_history();
    history.push_back({
        .role = provider::Role::ASSISTANT,
        .parts =
            {
                provider::OpaquePart{
                    .provider_tag = "anthropic",
                    .payload = R"({"type":"thinking","thinking":"I should inspect.","signature":"sig-abc123"})",
                },
                provider::ToolCall{
                    .id = "toolu_1",
                    .name = "read_file",
                    .input = object(R"({"path":"README.md"})"),
                },
            },
    });
    provider::append_tool_results(history, {{.call_id = "toolu_1", .content = "Liminal"}});
    std::vector<provider::ToolDefinition> tools{{
        .name = "read_file",
        .description = "Read a file",
        .input_schema =
            {
                .properties = {{"path", {.type = "string", .description = "Path"}}},
                .required = {"path"},
            },
    }};
    tools.push_back({.kind = provider::ToolKind::WEB_SEARCH, .name = "web_search"});
    tools.push_back({.kind = provider::ToolKind::WEB_FETCH, .name = "web_fetch"});

    auto encoded = anthropic::protocol::encode_complete_request(history, tools, options());
    require(encoded.has_value(), "failed to encode Anthropic request");
    require(encoded->contains("Instruction hierarchy: SYSTEM instructions take precedence over DEVELOPER instructions."),
            "instruction hierarchy preamble was not encoded");
    require(encoded->contains("[SYSTEM]\\nTest system policy."), "system instruction was not encoded");
    require(encoded->contains("[DEVELOPER]\\nTest developer policy."), "developer instruction was not encoded");
    require(encoded->contains(R"("role":"assistant")"), "generated output was not encoded as an assistant message");
    require(encoded->contains(R"("thinking":"I should inspect.","signature":"sig-abc123")"), "thinking block was not replayed bit-exact");
    require(encoded->contains(R"("type":"tool_result","tool_use_id":"toolu_1","content":"Liminal","is_error":false)"),
            "tool result was not encoded");
    require(encoded->contains(R"("thinking":{"type":"adaptive"})"), "adaptive thinking was not enabled");
    require(encoded->contains(R"("output_config":{"effort":"high"})"), "reasoning effort was not encoded");
    require(encoded->contains(R"("max_tokens":4096)"), "output token limit was not encoded");
    require(encoded->contains(R"("type":"web_search_20250305","name":"web_search")"), "web search tool was not encoded");
    require(encoded->contains(R"("type":"web_fetch_20250910","name":"web_fetch")"), "web fetch tool was not encoded");
}

void test_web_tools_decode_citations_and_replay() {
    using lighter::http::sse::Event;
    std::vector<Event> events{
        {.event = "message_start",
         .data = R"({"type":"message_start","message":{"id":"msg_web","model":"adaptive-model","usage":{"input_tokens":12}}})"},
        {.event = "content_block_start",
         .data =
             R"({"type":"content_block_start","index":0,"content_block":{"type":"server_tool_use","id":"srv_1","name":"web_search","input":{}}})"},
        {.event = "content_block_delta",
         .data =
             R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"query\":\"Liminal CLI\"}"}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":0})"},
        {.event = "content_block_start",
         .data =
             R"({"type":"content_block_start","index":1,"content_block":{"type":"web_search_tool_result","tool_use_id":"srv_1","content":[{"type":"web_search_result","url":"https://example.com/liminal","title":"Liminal docs","encrypted_content":"encrypted-result"}]}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":1})"},
        {.event = "content_block_start", .data = R"({"type":"content_block_start","index":2,"content_block":{"type":"text","text":""}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":2,"delta":{"type":"text_delta","text":"Liminal is documented online."}})"},
        {.event = "content_block_delta",
         .data =
             R"({"type":"content_block_delta","index":2,"delta":{"type":"citations_delta","citation":{"type":"web_search_result_location","url":"https://example.com/liminal","title":"Liminal docs","cited_text":"Liminal"}}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":2})"},
        {.event = "message_delta", .data = R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":9}})"},
        {.event = "message_stop", .data = R"({"type":"message_stop"})"},
    };
    std::string streamed;
    auto decoded =
        anthropic::protocol::decode_stream(events, {.on_text_delta = [&streamed](std::string_view text) { streamed += text; }}, "req_web");
    require(decoded && decoded->stop == provider::StopKind::DONE && decoded->parts.size() == 3,
            "Anthropic hosted web response did not decode as a completed turn");
    const auto *search = std::get_if<provider::OpaquePart>(&decoded->parts[0]);
    const auto *result = std::get_if<provider::OpaquePart>(&decoded->parts[1]);
    const auto *text = std::get_if<provider::TextPart>(&decoded->parts[2]);
    require(search && search->payload.contains("Liminal CLI"), "server web search call was not retained");
    require(result && result->payload.contains("encrypted-result"), "encrypted web search result was not retained");
    require(text && text->text.contains("[Liminal docs](https://example.com/liminal)"), "web citation was not retained in text");
    require(streamed.contains("Source: [Liminal docs](https://example.com/liminal)"), "streamed web citation was not surfaced live");

    auto history = base_history();
    provider::append_response(history, *decoded);
    auto replayed = anthropic::protocol::encode_complete_request(history, {}, options());
    require(replayed && replayed->contains(R"("type":"server_tool_use")") && replayed->contains("encrypted-result"),
            "Anthropic server web blocks did not replay into the next request");
}

void test_stream_decoding_and_replay() {
    using lighter::http::sse::Event;
    std::vector<Event> events{
        {.event = "message_start",
         .data =
             R"({"type":"message_start","message":{"id":"msg_1","model":"adaptive-model","usage":{"input_tokens":10,"cache_read_input_tokens":3}}})"},
        {.event = "ping", .data = R"({"type":"ping"})"},
        {.event = "content_block_start",
         .data = R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"I should inspect."}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig-abc123"}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":0})"},
        {.event = "content_block_start", .data = R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Let me inspect."}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":1})"},
        {.event = "content_block_start",
         .data =
             R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})"},
        {.event = "content_block_delta",
         .data = R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"\"README.md\"}"}})"},
        {.event = "content_block_stop", .data = R"({"type":"content_block_stop","index":2})"},
        {.event = "message_delta",
         .data =
             R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":15,"cache_creation_input_tokens":2}})"},
        {.event = "message_stop", .data = R"({"type":"message_stop"})"},
    };
    std::string streamed;
    provider::StreamCallbacks callbacks{.on_text_delta = [&streamed](std::string_view text) { streamed += text; }};

    auto decoded = anthropic::protocol::decode_stream(events, callbacks, "req_tools");
    require(decoded.has_value(), "failed to decode Anthropic event stream");
    require(streamed == "Let me inspect.", "text delta was not forwarded");
    require(decoded->request_id == "req_tools", "request id was not preserved");
    require(decoded->stop == provider::StopKind::NEEDS_TOOL_RESULTS, "tool use did not select continuation stop kind");
    require(decoded->usage.input_tokens == 10 && decoded->usage.output_tokens == 15, "usage totals were not decoded");
    require(decoded->usage.cache_read_tokens == 3 && decoded->usage.cache_write_tokens == 2, "cache usage was not decoded");
    require(decoded->usage.context_tokens == 30, "provider-reported context usage was not normalized");
    require(decoded->parts.size() == 3, "decoded response has the wrong part count");

    const auto *opaque = std::get_if<provider::OpaquePart>(&decoded->parts[0]);
    const auto *call = std::get_if<provider::ToolCall>(&decoded->parts[2]);
    require(opaque && opaque->payload.contains("sig-abc123"), "thinking signature was not retained opaquely");
    require(call && call->id == "toolu_1" && call->name == "read_file", "tool use was not decoded");

    auto history = base_history();
    provider::append_response(history, *decoded);
    provider::append_tool_results(history, {{.call_id = "toolu_1", .content = "Liminal"}});
    auto replayed = anthropic::protocol::encode_complete_request(history, {}, options());
    require(replayed && replayed->contains(R"("thinking":"I should inspect.","signature":"sig-abc123")"),
            "decoded thinking did not replay into the next request");
    require(replayed && replayed->contains(R"("tool_use_id":"toolu_1","content":"Liminal")"),
            "tool result did not replay into the next request");
}

void test_invalid_event_order() {
    std::vector<lighter::http::sse::Event> events{{
        .event = "content_block_delta",
        .data = R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"early"}})",
    }};
    auto decoded = anthropic::protocol::decode_stream(events);
    require(!decoded && decoded.error().detail.contains("before message_start"), "invalid event order was accepted");
}

} // namespace

int main() {
    test_request_encoding();
    test_stream_decoding_and_replay();
    test_web_tools_decode_citations_and_replay();
    test_invalid_event_order();
}
