#include "sse.h"

#include <charconv>
#include <utility>

namespace lighter::http::sse {

namespace {

constexpr std::string_view k_bom = "\xEF\xBB\xBF";

} // namespace

void Parser::feed(std::string_view bytes) {
    if (!seen_first_byte && !bytes.empty()) {
        // A single leading UTF-8 BOM must be ignored (spec: "one leading
        // U+FEFF"). It can only occur at the very start of the stream, but may
        // be split across feed() calls; buffer via `pending` handles that case
        // because we only strip once a complete line is assembled... except a
        // BOM does not end a line. Strip eagerly instead: accumulate up to 3
        // bytes in `pending` while they are a BOM prefix.
        if (pending.size() < k_bom.size() && k_bom.starts_with(pending)) {
            while (!bytes.empty() && pending.size() < k_bom.size() && bytes.front() == k_bom[pending.size()]) {
                pending.push_back(bytes.front());
                bytes.remove_prefix(1);
            }
            if (pending == k_bom) {
                pending.clear();
                seen_first_byte = true;
            } else if (!bytes.empty()) {
                // definitely not a BOM; keep accumulated bytes as line content
                seen_first_byte = true;
            }
        } else {
            seen_first_byte = true;
        }
    }

    for (usize i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (skip_next_lf) {
            skip_next_lf = false;
            if (c == '\n') {
                continue;
            }
        }
        if (c == '\n') {
            process_line(pending);
            pending.clear();
        } else if (c == '\r') {
            process_line(pending);
            pending.clear();
            skip_next_lf = true; // swallow the LF of a CRLF pair, even across chunks
        } else {
            pending.push_back(c);
        }
    }
}

void Parser::process_line(std::string_view line) {
    if (line.empty()) {
        dispatch();
        return;
    }
    if (line.front() == ':') {
        // comment line (used as keep-alive by most servers)
        return;
    }

    std::string_view field = line;
    std::string_view value;
    if (auto colon = line.find(':'); colon != std::string_view::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        // exactly one leading space is stripped from the value
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "data") {
        if (has_data) {
            data.push_back('\n');
        }
        data.append(value);
        has_data = true;
    } else if (field == "event") {
        event_type.assign(value);
    } else if (field == "id") {
        // an id containing NUL is ignored per spec
        if (value.find('\0') == std::string_view::npos) {
            last_id.assign(value);
        }
    } else if (field == "retry") {
        u64 ms = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
        if (ec == std::errc{} && ptr == value.data() + value.size()) {
            retry = ms;
        }
        // non-numeric retry values are ignored per spec
    }
    // unknown fields are ignored per spec
}

void Parser::dispatch() {
    // a blank line with no accumulated data dispatches nothing, but the event
    // type buffer still resets (spec steps for "dispatch the event")
    if (!has_data) {
        event_type.clear();
        return;
    }

    Event out;
    out.event = event_type.empty() ? std::string("message") : std::move(event_type);
    out.data = std::move(data);
    out.id = last_id;
    ready.push_back(std::move(out));

    event_type.clear();
    data.clear();
    has_data = false;
}

std::optional<Event> Parser::next() {
    if (ready.empty()) {
        return std::nullopt;
    }
    Event out = std::move(ready.front());
    ready.pop_front();
    return out;
}

} // namespace lighter::http::sse
