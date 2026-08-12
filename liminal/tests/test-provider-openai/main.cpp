#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/http/sse.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>

#include <liminal/provider/detail/completion_retry.h>
#include <liminal/provider/detail/openai_compaction.h>
#include <liminal/provider/detail/openai_protocol.h>

namespace {

using namespace lighter::types;
using namespace liminal;
using namespace std::chrono_literals;
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
                provider::TextPart{.text = "I will inspect."},
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
        .phase = provider::MessagePhase::COMMENTARY,
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
    tools.push_back({.kind = provider::ToolKind::WEB_SEARCH, .name = "web_search"});
    tools.push_back({.kind = provider::ToolKind::WEB_FETCH, .name = "web_fetch"});

    auto encoded = openai::protocol::encode_complete_request(history, tools, options());
    require(encoded.has_value(), "failed to encode OpenAI request");
    require(encoded->contains(R"("store":false)"), "request must disable storage");
    require(encoded->contains(R"("reasoning.encrypted_content")"), "request must include encrypted reasoning");
    require(encoded->contains(R"("instructions":"Test system policy.")") && !encoded->contains(R"("role":"system")"),
            "system instructions must be lifted into the top-level field the Codex backend accepts");
    require(encoded->contains(R"("role":"developer")") && encoded->contains("Test developer policy."),
            "developer instruction was not encoded explicitly");
    require(encoded->contains(R"("role":"assistant","content":[{"type":"output_text","text":"I will inspect."}])") &&
                encoded->contains(R"("phase":"commentary")"),
            "generated text was not encoded as assistant output");
    require(encoded->contains(R"("encrypted_content":"encrypted-reasoning")"), "encrypted reasoning was not replayed");
    require(encoded->contains(R"("type":"function_call_output","call_id":"call_1","output":"Liminal")"), "tool result was not replayed");
    require(encoded->contains(R"("strict":false)"),
            "tool schemas must stay non-strict so optional parameters survive Responses schema validation");
    require(encoded->contains(R"("additionalProperties":false)"), "tool schema must reject extra properties");
    require(encoded->contains(R"("type":"web_search")"), "hosted web search tool was not encoded");
    require(!encoded->contains(R"("name":"web_fetch")"), "OpenAI must not receive a redundant standalone web fetch tool");
    require(encoded->contains(R"("reasoning":{"effort":"high"})"), "reasoning effort was not encoded");
    require(encoded->contains(R"("max_output_tokens":4096)"), "output token limit was not encoded");
}

void test_web_search_decoding_citations_and_replay() {
    using lighter::http::sse::Event;
    std::vector<Event> events{
        {.event = "response.created", .data = R"({"response":{"id":"resp_web","model":"manual-model","status":"in_progress"}})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":0,"item":{"type":"web_search_call","id":"ws_1","status":"completed","action":{"type":"search","query":"Liminal CLI"}}})"},
        {.event = "response.output_text.delta", .data = R"({"delta":"Liminal is documented online."})"},
        {.event = "response.output_text.annotation.added",
         .data =
             R"({"annotation":{"type":"url_citation","start_index":0,"end_index":7,"url":"https://example.com/liminal","title":"Liminal docs"}})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":1,"item":{"type":"message","id":"msg_web","role":"assistant","status":"completed","content":[{"type":"output_text","text":"Liminal is documented online.","annotations":[{"type":"url_citation","start_index":0,"end_index":7,"url":"https://example.com/liminal","title":"Liminal docs"}]}]}})"},
        {.event = "response.completed",
         .data =
             R"({"response":{"id":"resp_web","model":"manual-model","status":"completed","usage":{"input_tokens":8,"output_tokens":6}}})"},
    };
    std::string streamed;
    std::vector<provider::OutputItem> outputs;
    std::vector<provider::OutputItemHeader> started;
    provider::StreamCallbacks callbacks{
        .on_item_started = [&started](const provider::OutputItemHeader &item) { started.push_back(item); },
        .on_assistant_text_delta = [&streamed](const provider::OutputItemId &, std::string_view text) { streamed += text; },
        .on_item_completed = [&outputs](const provider::OutputItem &item) { outputs.push_back(item); },
    };
    auto decoded = openai::protocol::decode_stream(events, callbacks, "req_web");
    require(decoded && decoded->stop == provider::StopKind::DONE && outputs.size() == 2 && started.size() == outputs.size(),
            "OpenAI hosted web search response did not decode as a completed turn");
    const auto *search = std::get_if<provider::ProviderOpaqueItem>(&outputs[0]);
    const auto *text = std::get_if<provider::AssistantMessageItem>(&outputs[1]);
    require(search && search->part.payload.contains("Liminal CLI"), "web search call was not retained for replay");
    require(text && text->parts[0].text.contains("[Liminal docs](https://example.com/liminal)"), "URL citation was not retained in text");
    require(streamed.contains("Source: [Liminal docs](https://example.com/liminal)"), "streamed citation was not surfaced live");

    auto history = base_history();
    for (const auto &output : outputs) provider::append_output_item(history, output);
    auto replayed = openai::protocol::encode_complete_request(history, {}, options());
    require(replayed && replayed->contains(R"("type":"web_search_call")") && replayed->contains("Liminal CLI"),
            "web search call did not replay into the next request");
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
             R"({"output_index":1,"item":{"type":"message","id":"msg_1","role":"assistant","status":"completed","phase":"commentary","content":[{"type":"output_text","text":"Let me inspect."}]}})"},
        {.event = "response.output_item.done",
         .data =
             R"({"output_index":2,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read_file","arguments":"{\"path\":\"README.md\"}","status":"completed"}})"},
        {.event = "response.completed",
         .data =
             R"({"response":{"id":"resp_1","model":"manual-model","status":"completed","usage":{"input_tokens":20,"output_tokens":15,"total_tokens":36,"input_tokens_details":{"cached_tokens":4},"output_tokens_details":{"reasoning_tokens":5}}}})"},
    };
    std::string streamed;
    std::vector<provider::OutputItem> outputs;
    std::vector<provider::OutputItemHeader> started;
    provider::StreamCallbacks callbacks{
        .on_item_started = [&started](const provider::OutputItemHeader &item) { started.push_back(item); },
        .on_assistant_text_delta = [&streamed](const provider::OutputItemId &, std::string_view text) { streamed += text; },
        .on_item_completed = [&outputs](const provider::OutputItem &item) { outputs.push_back(item); },
    };

    auto decoded = openai::protocol::decode_stream(events, callbacks, "req_tools");
    require(decoded.has_value(), "failed to decode OpenAI event stream");
    require(streamed == "Let me inspect.", "text delta was not forwarded");
    require(decoded->request_id == "req_tools", "request id was not preserved");
    require(decoded->stop == provider::StopKind::NEEDS_TOOL_RESULTS, "tool call did not select continuation stop kind");
    require(decoded->usage.input_tokens == 20 && decoded->usage.output_tokens == 15, "usage totals were not decoded");
    require(decoded->usage.cache_read_tokens == 4 && decoded->usage.reasoning_tokens == 5, "usage details were not decoded");
    require(decoded->usage.context_tokens == 36, "provider-reported context usage was not normalized");
    require(outputs.size() == 3 && started.size() == outputs.size(), "decoded response has the wrong output item lifecycle");

    const auto *opaque = std::get_if<provider::ProviderOpaqueItem>(&outputs[0]);
    const auto *message = std::get_if<provider::AssistantMessageItem>(&outputs[1]);
    const auto *call = std::get_if<provider::ToolCallItem>(&outputs[2]);
    require(opaque && opaque->part.payload.contains("encrypted-reasoning"), "encrypted reasoning was not retained opaquely");
    require(opaque->id.value == "rs_1" && message && message->id.value == "msg_1" && message->phase == provider::MessagePhase::COMMENTARY,
            "output item identity or message phase was not preserved");
    require(call && call->call.id == "call_1" && call->call.name == "read_file", "function call was not decoded");

    auto history = base_history();
    for (const auto &output : outputs) provider::append_output_item(history, output);
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

void test_completion_retry_is_scriptable() {
    usize stream_calls = 0;
    provider::detail::CompletionAttempts attempts{
        .stream = [&stream_calls](const std::string &body, const provider::StreamCallbacks &,
                                  bool &) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            ++stream_calls;
            if (stream_calls == 1) {
                co_await lighter::fail(Error::http_status(429, "rate_limit_error", "slow down", "req_1", 7ms));
            }
            require(body == "encoded request", "retry changed the request body");
            co_return provider::ProviderCallCompletion{.stop = provider::StopKind::DONE, .model = "test-model"};
        },
        .sleep = [](std::chrono::milliseconds delay) -> lighter::Task<> {
            require(delay == 7ms, "retry-after delay was not honored");
            co_return;
        },
    };

    auto task = provider::detail::complete_with_retry(attempts, "encoded request", {}, 2, 500ms);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();
    auto result = task.result();

    require(result && result->model == "test-model" && stream_calls == 2, "retry did not return the successful attempt");
}

void test_completion_does_not_retry_visible_output() {
    usize sleep_calls = 0;
    provider::detail::CompletionAttempts attempts{
        .stream = [](const std::string &, const provider::StreamCallbacks &,
                     bool &output_emitted) -> lighter::Task<provider::ProviderCallCompletion, Error> {
            output_emitted = true;
            co_await lighter::fail(Error::http_status(500, "server_error", "failed after text", "req_1"));
        },
        .sleep = [&sleep_calls](std::chrono::milliseconds) -> lighter::Task<> {
            ++sleep_calls;
            co_return;
        },
    };

    auto task = provider::detail::complete_with_retry(attempts, "encoded request", {}, 2, 1ms);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();
    auto result = task.result();

    require(!result && result.error().status == 500 && sleep_calls == 0, "visible output failure was retried or lost");
}

void test_compaction_falls_back_for_missing_endpoint() {
    usize local_calls = 0;
    usize sleep_calls = 0;
    openai::detail::CompactionAttempts attempts{
        .remote = [](const std::string &) -> lighter::Task<std::vector<provider::OpaquePart>, Error> {
            co_await lighter::fail(Error::http_status(404, "not_found", "missing", "req_compact"));
        },
        .local = [&local_calls](provider::History &history, std::string_view instructions) -> lighter::Task<void, Error> {
            ++local_calls;
            require(instructions == "keep decisions", "fallback received the wrong instructions");
            history.resize(1);
            provider::append_user(history, "SUMMARY");
            co_return;
        },
        .sleep = [&sleep_calls](std::chrono::milliseconds) -> lighter::Task<> {
            ++sleep_calls;
            co_return;
        },
    };
    auto history = base_history();

    auto task = openai::detail::compact_with_retry(attempts, history, 2, "compact body", "keep decisions", 2, 1ms);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();
    auto result = task.result();

    require(result.has_value() && history.size() == 2 && local_calls == 1 && sleep_calls == 0,
            "missing compact endpoint did not use local fallback");
}

void test_native_compaction_retries_and_replaces_conversation() {
    usize remote_calls = 0;
    usize local_calls = 0;
    usize sleep_calls = 0;
    openai::detail::CompactionAttempts attempts{
        .remote = [&remote_calls](const std::string &) -> lighter::Task<std::vector<provider::OpaquePart>, Error> {
            ++remote_calls;
            if (remote_calls == 1) {
                co_await lighter::fail(Error::http_status(500, "server_error", "retry", "req_compact", 3ms));
            }
            co_return std::vector<provider::OpaquePart>{{.provider_tag = "openai", .payload = "encrypted-compaction"}};
        },
        .local = [&local_calls](provider::History &, std::string_view) -> lighter::Task<void, Error> {
            ++local_calls;
            co_return;
        },
        .sleep = [&sleep_calls](std::chrono::milliseconds delay) -> lighter::Task<> {
            ++sleep_calls;
            require(delay == 3ms, "compact retry-after delay was not honored");
            co_return;
        },
    };
    auto history = base_history();

    auto task = openai::detail::compact_with_retry(attempts, history, 2, "compact body", "keep decisions", 2, 1ms);
    lighter::EventLoop loop;
    loop.schedule(task);
    loop.run();
    auto result = task.result();

    require(result.has_value(), "native compaction retry failed");
    require(history.size() == 3 && history[0].role == provider::Role::SYSTEM && history[1].role == provider::Role::DEVELOPER,
            "native compaction did not preserve the instruction prefix");
    const auto *opaque = std::get_if<provider::OpaquePart>(&history.back().parts.front());
    require(opaque && opaque->payload == "encrypted-compaction", "native compaction did not replace the conversation");
    require(remote_calls == 2 && local_calls == 0 && sleep_calls == 1, "native compaction used the wrong retry path");
}

} // namespace

int main() {
    test_request_encoding();
    test_stream_decoding_and_replay();
    test_web_search_decoding_citations_and_replay();
    test_invalid_event_order();
    test_invalid_instruction_order();
    test_completion_retry_is_scriptable();
    test_completion_does_not_retry_visible_output();
    test_compaction_falls_back_for_missing_endpoint();
    test_native_compaction_retries_and_replaces_conversation();
}
