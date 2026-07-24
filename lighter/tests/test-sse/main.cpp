#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/codec/json/json.h>
#include <lighter/http/sse.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;
using http::sse::Event;
using http::sse::Parser;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::vector<Event> drain(Parser &parser) {
    std::vector<Event> events;
    while (auto event = parser.next()) {
        events.push_back(*std::move(event));
    }
    return events;
}

/// Feed the whole stream in one chunk, then in chunks of every size from 1 up,
/// requiring identical output. This is the core robustness property: servers
/// and TCP fragment streams at arbitrary byte boundaries.
std::vector<Event> parse_any_split(std::string_view stream) {
    Parser whole;
    whole.feed(stream);
    auto expected = drain(whole);

    for (usize chunk = 1; chunk <= stream.size(); ++chunk) {
        Parser parser;
        for (usize off = 0; off < stream.size(); off += chunk) {
            parser.feed(stream.substr(off, chunk));
        }
        auto got = drain(parser);
        require(got.size() == expected.size(), "chunk size " + std::to_string(chunk) + ": event count mismatch");
        for (usize i = 0; i < got.size(); ++i) {
            require(got[i].event == expected[i].event && got[i].data == expected[i].data && got[i].id == expected[i].id,
                    "chunk size " + std::to_string(chunk) + ": event " + std::to_string(i) + " mismatch");
        }
    }
    return expected;
}

// -- typical agentic CLI payloads ------------------------------------------

/// OpenAI-compatible chat completion chunk (the shape liminal will decode per event)
struct DeltaChunk {
    struct Choice {
        struct Delta {
            std::optional<std::string> role;
            std::optional<std::string> content;
        };
        Delta delta;
        std::optional<std::string> finish_reason;
        u32 index = 0;
    };
    std::string id;
    std::vector<Choice> choices;
};

void test_openai_style_stream() {
    // data-only events, keep-alive comment, [DONE] sentinel — the wire format
    // of OpenAI-compatible endpoints (which most local gateways mimic)
    const std::string_view stream = ": ping\n"
                                    "\n"
                                    "data: {\"id\":\"c1\",\"choices\":[{\"delta\":{\"role\":\"assistant\"},\"index\":0}]}\n"
                                    "\n"
                                    "data: {\"id\":\"c1\",\"choices\":[{\"delta\":{\"content\":\"Hel\"},\"index\":0}]}\n"
                                    "\n"
                                    "data: {\"id\":\"c1\",\"choices\":[{\"delta\":{\"content\":\"lo\"},\"index\":0}]}\n"
                                    "\n"
                                    "data: {\"id\":\"c1\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\",\"index\":0}]}\n"
                                    "\n"
                                    "data: [DONE]\n"
                                    "\n";
    auto events = parse_any_split(stream);
    require(events.size() == 5, "openai stream event count mismatch");

    // the [DONE] sentinel is application-level: parser must pass it through untouched
    require(events.back().data == "[DONE]", "sentinel event must pass through verbatim");

    // decode the JSON payloads and reassemble the streamed message
    std::string content;
    std::optional<std::string> finish;
    for (const auto &event : events) {
        if (event.data == "[DONE]") {
            continue;
        }
        require(event.event == "message", "data-only events default to type 'message'");
        auto chunk = codec::json::parse<DeltaChunk>(event.data);
        require(static_cast<bool>(chunk), chunk ? std::string() : std::string(chunk.error().message()));
        for (const auto &choice : chunk->choices) {
            if (choice.delta.content) {
                content += *choice.delta.content;
            }
            if (choice.finish_reason) {
                finish = choice.finish_reason;
            }
        }
    }
    require(content == "Hello", "reassembled streamed content mismatch");
    require(finish == std::optional<std::string>("stop"), "finish_reason not seen");
}

void test_anthropic_style_stream() {
    // named event types with per-event JSON — the Anthropic Messages API shape
    const std::string_view stream = "event: message_start\n"
                                    "data: {\"type\":\"message_start\"}\n"
                                    "\n"
                                    "event: content_block_delta\n"
                                    "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hi\"}}\n"
                                    "\n"
                                    "event: content_block_delta\n"
                                    "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"!\"}}\n"
                                    "\n"
                                    "event: message_stop\n"
                                    "data: {\"type\":\"message_stop\"}\n"
                                    "\n";
    auto events = parse_any_split(stream);
    require(events.size() == 4, "anthropic stream event count mismatch");
    require(events[0].event == "message_start", "event type mismatch");
    require(events[1].event == "content_block_delta" && events[2].event == "content_block_delta", "delta event types mismatch");
    require(events[3].event == "message_stop", "terminal event type mismatch");

    struct BlockDelta {
        std::string type;
        struct Delta {
            std::optional<std::string> text;
        };
        std::optional<Delta> delta;
    };
    std::string text;
    for (const auto &event : events) {
        if (event.event != "content_block_delta") {
            continue;
        }
        auto delta = codec::json::parse<BlockDelta>(event.data);
        require(static_cast<bool>(delta), "delta payload failed to decode");
        if (delta->delta && delta->delta->text) {
            text += *delta->delta->text;
        }
    }
    require(text == "Hi!", "anthropic delta reassembly mismatch");
}

void test_multiline_data() {
    // multi-line data joins with '\n' — used by tools that stream diffs/code blocks
    const std::string_view stream = "data: line one\n"
                                    "data: line two\n"
                                    "data:\n"
                                    "data: after blank\n"
                                    "\n";
    auto events = parse_any_split(stream);
    require(events.size() == 1, "multiline event count mismatch");
    require(events[0].data == "line one\nline two\n\nafter blank", "multiline data join mismatch");
}

void test_line_endings_and_bom() {
    // CRLF and lone CR are valid line terminators; leading BOM must be stripped
    const std::string_view stream = "\xEF\xBB\xBF"
                                    "data: crlf\r\n"
                                    "\r\n"
                                    "data: lone-cr\r"
                                    "\r"
                                    "data: lf\n"
                                    "\n";
    auto events = parse_any_split(stream);
    require(events.size() == 3, "line ending event count mismatch");
    require(events[0].data == "crlf" && events[1].data == "lone-cr" && events[2].data == "lf", "line ending data mismatch");
}

void test_id_retry_and_edge_fields() {
    using namespace std::string_view_literals;
    // sv suffix: embedded NUL below must survive, a const char* would truncate
    const auto stream = "retry: 3000\n"
                        "id: 41\n"
                        "data: first\n"
                        "\n"
                        "data: second\n" // id persists across events
                        "\n"
                        "id: bad\0id\n" // NUL in id: ignored, keeps previous
                        "data: third\n"
                        "\n"
                        "retry: notanumber\n" // ignored
                        "event: only-type-no-data\n"
                        "\n"     // no data accumulated: nothing dispatched
                        "data\n" // field with no colon: empty value
                        "data: x\n"
                        "\n"sv;
    Parser parser;
    parser.feed(stream);
    auto events = drain(parser);

    require(events.size() == 4, "edge field event count mismatch, got " + std::to_string(events.size()));
    require(events[0].id == "41" && events[1].id == "41" && events[2].id == "41", "last_event_id persistence mismatch");
    require(parser.last_event_id() == "41", "parser last_event_id mismatch");
    require(parser.retry_ms() == std::optional<u64>(3000), "retry value mismatch");
    require(events[3].data == "\nx", "colon-less data field should contribute an empty line");
    require(events[3].event == "message", "event type must reset after the empty dispatch");
}

void test_incomplete_tail_not_dispatched() {
    // an unterminated event at end-of-stream must NOT be dispatched (spec);
    // matters when a connection drops mid-generation
    Parser parser;
    parser.feed("data: complete\n\ndata: trunca");
    auto events = drain(parser);
    require(events.size() == 1, "incomplete tail must not dispatch");
    require(events[0].data == "complete", "complete event data mismatch");

    // if the rest arrives after all (e.g. was just fragmentation), it dispatches
    parser.feed("ted\n\n");
    auto rest = drain(parser);
    require(rest.size() == 1 && rest[0].data == "truncated", "resumed tail dispatch mismatch");
}

void test_interleaved_keepalives() {
    // long tool executions make servers emit periodic comments; they must not
    // break data accumulation within an event
    const std::string_view stream = "data: part1\n"
                                    ": keep-alive 1\n"
                                    "data: part2\n"
                                    ": keep-alive 2\n"
                                    "\n";
    auto events = parse_any_split(stream);
    require(events.size() == 1, "keepalive event count mismatch");
    require(events[0].data == "part1\npart2", "comments must not interrupt data accumulation");
}

} // namespace

int main() {
    test_openai_style_stream();
    test_anthropic_style_stream();
    test_multiline_data();
    test_line_endings_and_bom();
    test_id_retry_and_edge_fields();
    test_incomplete_tail_not_dispatched();
    test_interleaved_keepalives();
    return 0;
}
