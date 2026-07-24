#pragma once

#include <optional>
#include <utility>

#include <lighter/http/sse.h>
#include <lighter/http/streaming.h>
#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

namespace lighter::http::sse {

/// Async SSE event source over a StreamingResponse.
///
/// next() returns the next event, nullopt when the server closed the stream
/// cleanly, or an Error on transport failure. Cancellation propagates through
/// next(); unwinding destroys the owned StreamingResponse, which aborts the
/// transfer - no explicit shutdown call exists or is needed.
struct EventStream {
    explicit EventStream(StreamingResponse response) noexcept : response(std::move(response)) {}

    Task<std::optional<Event>, Error> next() {
        while (true) {
            if (auto event = parser.next()) {
                co_return event;
            }

            auto chunk = co_await response.read_chunk().or_fail();
            if (chunk.empty()) {
                // clean end of body; an event lacking its terminating blank
                // line is dropped per spec
                co_return std::nullopt;
            }
            parser.feed(std::string_view(chunk.data(), chunk.size()));
            response.consume(chunk.size());
        }
    }

    /// Last seen `id:` value - reconnection metadata for the caller.
    std::string_view last_event_id() const noexcept { return parser.last_event_id(); }

    /// Last seen `retry:` value in milliseconds, if any.
    std::optional<u64> retry_ms() const noexcept { return parser.retry_ms(); }

    const StreamingResponse &http_response() const noexcept { return response; }

private:
    StreamingResponse response;
    Parser parser;
};

} // namespace lighter::http::sse
