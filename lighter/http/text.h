#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <lighter/encoding/utf8.h>
#include <lighter/http/streaming.h>
#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

namespace lighter::http {

/// UTF-8 text view over a StreamingResponse. read_chunk() hands out raw byte
/// windows that can end mid-character; next() re-chunks the body so every
/// returned piece contains only whole, well-formed sequences - a partial
/// sequence at a chunk boundary is carried until later bytes complete it, and
/// ill-formed bytes become U+FFFD (same policy as encoding::utf8::sanitize).
///
/// An empty view means the body ended cleanly; transport failures surface on
/// the Error channel. The view stays valid until the next next() call.
/// Dropping the object aborts the transfer, as with StreamingResponse.
///
/// Note: this assumes the body is UTF-8 (correct for JSON APIs and SSE). For
/// other charsets, transcode first - a Content-Type-driven decoder will land
/// in encoding/ later.
struct TextStream {
    explicit TextStream(StreamingResponse response) noexcept : response(std::move(response)) {}

    Task<std::string_view, Error> next() {
        buffer.clear();
        while (true) {
            auto chunk = co_await response.read_chunk().or_fail();
            if (chunk.empty()) {
                // a carried partial sequence can no longer complete: one U+FFFD
                sanitizer.finish(buffer);
                co_return std::string_view(buffer);
            }
            sanitizer.feed(std::string_view(chunk.data(), chunk.size()), buffer);
            response.consume(chunk.size());
            if (!buffer.empty()) {
                co_return std::string_view(buffer);
            }
            // the whole chunk was a partial sequence (1-3 bytes); keep reading
        }
    }

    const StreamingResponse &http_response() const noexcept { return response; }

private:
    StreamingResponse response;
    encoding::utf8::Sanitizer sanitizer;
    std::string buffer;
};

} // namespace lighter::http
