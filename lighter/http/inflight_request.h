#pragma once

#include <memory>
#include <string>
#include <vector>

#include <lighter/http/curl.h>
#include <lighter/http/request.h>
#include <lighter/http/response.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/types.hpp>

namespace lighter::http {

struct Manager;

} // namespace lighter::http

namespace lighter::http::detail {

/// Body plumbing for a streaming transfer. curl callbacks write into `buffer`
/// and signal the Events; Event::set() defers coroutine resumption to the
/// loop's check phase, so no consumer code ever runs inside a curl callback.
struct StreamConduit {
    /// on_write returns CURL_WRITEFUNC_PAUSE once this much is unconsumed
    constexpr static usize k_high_water = 256 * 1024;
    /// consume() unpauses once the backlog drops to this
    constexpr static usize k_low_water = 64 * 1024;

    Event readable;      // body bytes appended, or body finished
    Event headers_ready; // final headers complete, or transfer finished early
    std::string buffer;
    usize read_offset = 0;
    long pending_status = 0;   // parsed from the most recent status line
    bool intermediate = false; // current response is 1xx or a followed redirect
    bool headers_done = false;
    bool paused = false;
    bool body_done = false;

    usize buffered() const noexcept { return buffer.size() - read_offset; }

    void finalize_headers(Response &out) noexcept {
        if (headers_done) {
            return;
        }
        out.status = static_cast<int>(pending_status);
        headers_done = true;
        headers_ready.set();
    }
};

struct InflightRequest : RequestSettings {

    explicit InflightRequest(http::Request request) noexcept;
    InflightRequest(const InflightRequest &) = delete;
    InflightRequest &operator=(const InflightRequest &) = delete;
    InflightRequest(InflightRequest &&) = delete;
    InflightRequest &operator=(InflightRequest &&) = delete;

    bool fail(Error err) noexcept;
    bool fail(curl::EasyError code) noexcept;

    const http::RedirectPolicy &redirect_policy() const noexcept { return redirect_policy_value; }

    /// Switch body delivery from buffering into `out.body` to the conduit.
    /// Must be called before the transfer is registered with the multi handle.
    void enable_streaming();

    bool prepare() noexcept;
    bool bind_runtime(void *opaque) noexcept;
    void clear_runtime_binding() noexcept;
    Outcome<Response, Error, Cancellation> finish() noexcept;

    static usize on_write(char *data, usize size, usize count, void *userdata);
    static usize on_header(char *data, usize size, usize count, void *userdata);

    std::shared_ptr<SharedResources> shared;
    std::string method_name;
    std::string url_string;
    std::vector<QueryParam> query_params;
    std::string body_text;
    curl::EasyHandle easy;
    curl::SList header_lines;
    Response out{};
    Error result{};
    std::string final_url;
    /// non-null only for streaming transfers; owned here so curl callbacks
    /// (userdata = this) and the StreamingResponse share one object
    std::unique_ptr<StreamConduit> conduit;

private:
    bool apply_url() noexcept;
    bool apply_method() noexcept;
    bool apply_body() noexcept;
    bool apply_headers() noexcept;
    bool apply_cookies() noexcept;
    bool apply_user_agent() noexcept;
    bool apply_redirect() noexcept;
    bool apply_tls() noexcept;
    bool apply_proxy() noexcept;
    bool apply_timeout() noexcept;
    bool apply_curl_options() noexcept;
};

template <typename T>
bool easy_setopt(InflightRequest &request, CURLoption option, T value) noexcept {
    if (auto err = curl::setopt(request.easy.get(), option, value); !curl::ok(err)) {
        return request.fail(err);
    }
    return true;
}

/// Shared ownership hub for one transfer. libcurl callbacks, the Manager's
/// completion drain, and the awaiting coroutine (or StreamingResponse) all
/// reach the transfer through this object; whoever tears down last wins.
struct InflightRequestState : std::enable_shared_from_this<InflightRequestState> {
    explicit InflightRequestState(http::Request req) noexcept : request(std::move(req)) {}

    Manager *mgr = nullptr;
    InflightRequest request;
    IoOp *awaiter = nullptr;
    bool registered = false;
    bool completed = false;
    bool request_released = false;

    /// Bind the runtime opaque, hand the easy handle to the multi, and arm the
    /// timeout. On failure sets `completed` with the Error staged in `request`.
    void start_transfer() noexcept;

    void detach_from_multi() noexcept;
    void release_request() noexcept;

    void complete(Error err, bool resume) noexcept;

    void complete(curl::EasyError code, bool resume) noexcept { complete(Error::from_curl(code), resume); }
};

} // namespace lighter::http::detail
