#include "streaming.h"

#include <algorithm>
#include <utility>

#include <lighter/http/inflight_request.h>
#include <lighter/http/manager.h>
#include <lighter/http/request.h>
#include <lighter/http/util.h>

namespace lighter::http {

namespace detail {

namespace {

/// Aborts the transfer if execute_stream_request unwinds - error path or
/// cancellation while waiting for headers - before ownership moves into the
/// returned StreamingResponse.
struct StreamGuard {
    InflightRequestRef state;

    ~StreamGuard() {
        if (!state) {
            return;
        }
        state->detach_from_multi();
        state->release_request();
        state->completed = true;
    }

    void release() noexcept { state.reset(); }
};

Error transport_error(const InflightRequest &request) {
    if (request.result.kind != ErrorKind::CURL || !curl::ok(request.result.curl_code)) {
        return request.result;
    }
    return Error::invalid_request("stream transfer ended unexpectedly");
}

} // namespace

Task<StreamingResponse, Error> execute_stream_request(http::Request request, EventLoop &loop) {
    // libcurl callbacks keep `userdata = this`, so the prepared request must stay at a stable
    // address for the rest of its lifetime.
    auto state = make_inflight_request_state(std::move(request));
    state->request.enable_streaming();
    if (!state->request.prepare()) {
        co_await fail(std::move(state->request.result));
    }

    auto manager = Manager::try_for_loop(loop);
    if (!manager) {
        co_await fail(std::move(manager.error()));
    }

    state->mgr = &manager->get();
    state->start_transfer();
    if (state->completed && !state->request.conduit->headers_done) {
        co_await fail(transport_error(state->request));
    }

    StreamGuard guard{state};
    auto &conduit = *state->request.conduit;

    // Resolves on final headers, transfer failure, or early completion.
    // Cancellation while suspended here propagates out and StreamGuard
    // aborts the transfer.
    co_await conduit.headers_ready.wait();

    if (!conduit.headers_done) {
        // transfer ended before final headers arrived (connect/TLS failure...)
        co_await fail(transport_error(state->request));
    }

    guard.release();
    co_return StreamingResponse(std::move(state));
}

} // namespace detail

StreamingResponse::StreamingResponse(detail::InflightRequestRef state_ref) noexcept : state(std::move(state_ref)) {
    auto &request = state->request;
    status = request.out.status;
    // final headers are complete; move them out (later trailer lines would
    // append to the now-empty vector and are deliberately ignored)
    headers = std::move(request.out.headers);

    char *effective = nullptr;
    if (request.easy && curl::ok(curl::getinfo(request.easy.get(), CURLINFO_EFFECTIVE_URL, &effective)) && effective != nullptr) {
        url = effective;
    } else {
        url = request.final_url;
    }
}

StreamingResponse::~StreamingResponse() {
    if (!state) {
        return; // moved-from
    }
    state->detach_from_multi();
    state->release_request();
    state->completed = true;
}

std::optional<std::string_view> StreamingResponse::header_value(std::string_view name) const noexcept {
    for (const auto &item : headers) {
        if (detail::iequals(item.name, name)) {
            return item.value;
        }
    }
    return std::nullopt;
}

Task<StreamingResponse::Chunk, Error> StreamingResponse::read_chunk() {
    auto &st = *state;
    auto &conduit = *st.request.conduit;

    while (true) {
        if (conduit.buffered() > 0) {
            co_return Chunk(conduit.buffer.data() + conduit.read_offset, conduit.buffered());
        }

        if (st.completed || conduit.body_done) {
            if (st.request.result.kind != ErrorKind::CURL || !curl::ok(st.request.result.curl_code)) {
                co_await fail(st.request.result);
            }
            co_return Chunk{};
        }

        // Single-threaded loop: nothing can deliver between the checks above
        // and this reset, so no wakeup is lost.
        conduit.readable.reset();
        co_await conduit.readable.wait();
    }
}

void StreamingResponse::consume(usize n) {
    auto &st = *state;
    auto &conduit = *st.request.conduit;

    const auto released = std::min(n, conduit.buffered());
    // Compact eagerly: consumed bytes leave the buffer, so its size stays
    // bounded by high-water + one curl burst and appends never outgrow the
    // reservation (keeps unconsumed chunk views stable across appends).
    conduit.buffer.erase(0, conduit.read_offset + released);
    conduit.read_offset = 0;

    if (conduit.paused && conduit.buffered() <= detail::StreamConduit::k_low_water && !st.completed && !st.request_released &&
        st.request.easy) {
        conduit.paused = false;
        // may synchronously re-deliver the chunk rejected by on_write
        curl::easy_pause(st.request.easy.get(), CURLPAUSE_CONT);
        if (st.mgr) {
            // kick the multi so the unpaused transfer keeps progressing
            st.mgr->drive_timeout();
        }
    }
}

} // namespace lighter::http
