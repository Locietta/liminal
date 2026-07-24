#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/http/common.h>
#include <lighter/http/runtime.h>
#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

namespace lighter {

struct EventLoop;

} // namespace lighter

namespace lighter::http {

struct Request;
struct StreamingResponse;

} // namespace lighter::http

namespace lighter::http::detail {

Task<StreamingResponse, Error> execute_stream_request(http::Request request, EventLoop &loop);

} // namespace lighter::http::detail

namespace lighter::http {

/// A response whose body is still arriving. Produced by Request::stream(),
/// which resolves once the final response headers are complete - before the
/// body - so callers can branch on `status` without consuming anything.
///
/// Pull model: read_chunk() suspends until body bytes are available and
/// returns a view into the internal buffer; call consume() after processing.
/// Backpressure is automatic: the transfer pauses when the internal buffer
/// fills and resumes as the consumer drains it.
///
/// Cancellation-first lifecycle: there is no close() and no obligation to
/// drain. Dropping the object - including via cancellation unwinding a
/// coroutine that owns it - aborts the transfer immediately.
struct StreamingResponse {
    int status = 0;
    std::string url;
    std::vector<Header> headers;

    StreamingResponse(StreamingResponse &&) noexcept = default;
    StreamingResponse &operator=(StreamingResponse &&) noexcept = default;
    StreamingResponse(const StreamingResponse &) = delete;
    StreamingResponse &operator=(const StreamingResponse &) = delete;

    ~StreamingResponse();

    bool ok() const noexcept { return 200 <= status && status < 300; }

    std::optional<std::string_view> header_value(std::string_view name) const noexcept;

    using Chunk = std::span<const char>;

    /// Wait for body bytes. An empty chunk means the body ended cleanly;
    /// transport failures surface on the Error channel. The view stays valid
    /// until the next consume() call. `this` must outlive the returned Task.
    Task<Chunk, Error> read_chunk();

    /// Release `n` bytes of the last chunk; may resume a paused transfer.
    void consume(usize n);

private:
    friend Task<StreamingResponse, Error> detail::execute_stream_request(http::Request request, EventLoop &loop);

    explicit StreamingResponse(detail::InflightRequestRef state) noexcept;

    detail::InflightRequestRef state;
};

} // namespace lighter::http
