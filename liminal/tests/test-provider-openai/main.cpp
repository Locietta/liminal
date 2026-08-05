#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/http/sse.h>

#include <liminal/provider/detail/openai_protocol.h>

namespace {

using namespace lighter::types;
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

openai::ClientOptions options() {
    return {
        .model = "manual-model",
        .reasoning_effort = "high",
        .max_output_tokens = 4096,
    };
}

void test_request_encoding() {
    auto history = base_history();
    history.push_back({
        .role = provider::Role::ASSISTANT,
        .parts =
            {
                provider::OpaquePart{
                    .provider_tag = "openai",
                    .payload =
                        R"({"type":"reasoning","id":"rs_1","summary":[],"content":[],"encrypted_content":"encrypted-reasoning","status":"completed"})",
                },
                provider::ToolCall{
                    .id = "call_1",
                    .name = "read_file",
                    .input = object(R"({"path":"README.md"})"),
                },
            },
    });
    provider::append_tool_results(history, {{.call_id = "call_1", .content = "Liminal"}});
    std::vector<provider::ToolDefinition> tools{{
        .name = "read_file",
        .description = "Read a file",
        .input_schema =
            {
                .properties = {{"path", {.type = "string", .description = "Path"}}},
                .required = {"path"},
            },
    }};

    auto encoded = openai::protocol::encode_complete_request(history, tools, options());
    require(encoded.has_value(), "failed to encode OpenAI request");
    require(encoded->contains(R"("store":false)"), "request must disable storage");
    require(encoded->contains(R"("reasoning.encrypted_content")"), "request must include encrypted reasoning");
    require(encoded->contains(R"("role":"system")") && encoded->contains("Test system policy."),
            "system instruction was not encoded explicitly");
    require(encoded->contains(R"("role":"developer")") && encoded->contains("Test developer policy."),
            "developer instruction was not encoded explicitly");
    require(encoded->contains(R"("encrypted_content":"encrypted-reasoning")"), "encrypted reasoning was not replayed");
    require(encoded->contains(R"("type":"function_call_output","call_id":"call_1","output":"Liminal")"), "tool result was not replayed");
    require(encoded->contains(R"("strict":true)"), "tool schema must be strict");
    require(encoded->contains(R"("additionalProperties":false)"), "tool schema must reject extra properties");
    require(encoded->contains(R"("reasoning":{"effort":"high"})"), "reasoning effort was not encoded");
    require(encoded->contains(R"("max_output_tokens":4096)"), "output token limit was not encoded");
}

void test_stream_decoding_and_replay() {
    using lighter::http::sse::Event;
    std::vector<Event> events{
        {.event = "response.created", .data = R"({"response":{"id":"resp_1","model":"manual-model","status":"in_progress"}})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":0,"item":{"type":"reasoning","id":"rs_1","summary":[],"content":[],"encrypted_content":"encrypted-reasoning","status":"completed"}})"},
        {.event = "response.output_text.delta", .data = R"({"delta":"Let me inspect."})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":1,"item":{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"Let me inspect."}]}})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":2,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read_file","arguments":"{\"path\":\"README.md\"}","status":"completed"}})"},
        {.event = "response.completed",
         .data =
             R"({"response":{"id":"resp_1","model":"manual-model","status":"completed","usage":{"input_tokens":20,"output_tokens":15,"input_tokens_details":{"cached_tokens":4},"output_tokens_details":{"reasoning_tokens":5}}}})"},
    };
    std::string streamed;
    provider::StreamCallbacks callbacks{.on_text_delta = [&streamed](std::string_view text) { streamed += text; }};

    auto decoded = openai::protocol::decode_stream(events, callbacks, "req_tools");
    require(decoded.has_value(), "failed to decode OpenAI event stream");
    require(streamed == "Let me inspect.", "text delta was not forwarded");
    require(decoded->request_id == "req_tools", "request id was not preserved");
    require(decoded->stop == provider::StopKind::NEEDS_TOOL_RESULTS, "tool call did not select continuation stop kind");
    require(decoded->usage.input_tokens == 20 && decoded->usage.output_tokens == 15, "usage totals were not decoded");
    require(decoded->usage.cache_read_tokens == 4 && decoded->usage.reasoning_tokens == 5, "usage details were not decoded");
    require(decoded->parts.size() == 3, "decoded response has the wrong part count");

    const auto *opaque = std::get_if<provider::OpaquePart>(&decoded->parts[0]);
    const auto *call = std::get_if<provider::ToolCall>(&decoded->parts[2]);
    require(opaque && opaque->payload.contains("encrypted-reasoning"), "encrypted reasoning was not retained opaquely");
    require(call && call->id == "call_1" && call->name == "read_file", "function call was not decoded");

    auto history = base_history();
    provider::append_response(history, *decoded);
    provider::append_tool_results(history, {{.call_id = "call_1", .content = "Liminal"}});
    auto replayed = openai::protocol::encode_complete_request(history, {}, options());
    require(replayed && replayed->contains(R"("encrypted_content":"encrypted-reasoning")"),
            "decoded reasoning did not replay into the next request");
    require(replayed && replayed->contains(R"("call_id":"call_1","output":"Liminal")"), "tool output did not replay into the next request");
}

void test_invalid_event_order() {
    std::vector<lighter::http::sse::Event> events{{.event = "response.output_text.delta", .data = R"({"delta":"early"})"}};
    auto decoded = openai::protocol::decode_stream(events);
    require(!decoded && decoded.error().detail.contains("before response.created"), "invalid event order was accepted");
}

void test_invalid_instruction_order() {
    provider::History history;
    provider::append_user(history, "hello");
    provider::append_developer(history, "too late");

    auto encoded = openai::protocol::encode_complete_request(history, {}, options());
    require(!encoded && encoded.error().detail.contains("must precede"), "invalid instruction order was accepted");
}

} // namespace

int main() {
    test_request_encoding();
    test_stream_decoding_and_replay();
    test_invalid_event_order();
    test_invalid_instruction_order();
}
