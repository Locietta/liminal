#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include <lighter/types.hpp>

/// Incremental Server-Sent Events parser (WHATWG HTML spec, "9.2 Server-sent
/// events"). Pure state machine: bytes in via feed(), events out via next().
/// No I/O, no allocation beyond the event queue; safe to feed arbitrary chunk
/// splits, including mid-line and between CR and LF.
namespace lighter::http::sse {

struct Event {
    /// event type; "message" when the stream did not specify one
    std::string event;
    /// data lines joined with '\n'
    std::string data;
    /// last seen event id at dispatch time (persists across events)
    std::string id;
};

struct Parser {
    /// Consume a chunk of the stream. Complete events become available via next().
    void feed(std::string_view bytes);

    /// Pop the next dispatched event, or nullopt when all fed input is drained.
    std::optional<Event> next();

    /// Last seen `id:` field value (survives event boundaries, per spec).
    std::string_view last_event_id() const noexcept { return last_id; }

    /// Last valid `retry:` field value in milliseconds, if any was seen.
    std::optional<u64> retry_ms() const noexcept { return retry; }

private:
    void process_line(std::string_view line);
    void dispatch();

    std::deque<Event> ready;
    std::string pending; // partial line straddling a feed() boundary
    std::string event_type;
    std::string data;
    bool has_data = false;
    std::string last_id;
    std::optional<u64> retry;
    bool seen_first_byte = false; // for BOM stripping
    bool skip_next_lf = false;    // CRLF split across a feed() boundary
};

} // namespace lighter::http::sse
