#include <memory>
#include <source_location>

#include <lighter/async/io/awaiter.h>
#include <lighter/http/inflight_request.h>
#include <lighter/http/manager.h>
#include <lighter/http/request.h>
#include <lighter/http/runtime.h>

namespace lighter::http { namespace detail {

void InflightRequestState::detach_from_multi() noexcept {
    if (!registered || !mgr || request_released || !request.easy) {
        registered = false;
        return;
    }

    request.clear_runtime_binding();
    mgr->remove_request(request.easy.get());
    registered = false;
}

void InflightRequestState::release_request() noexcept {
    if (!request_released) {
        request.clear_runtime_binding();
        request.easy.reset();
        request_released = true;
    }
}

void InflightRequestState::start_transfer() noexcept {
    if (completed || request_released || !request.easy) {
        return;
    }

    if (!request.bind_runtime(inflight_request_opaque(shared_from_this()))) {
        if (request.result.kind == ErrorKind::CURL && curl::ok(request.result.curl_code)) {
            request.result = Error::invalid_request("request runtime binding failed");
        }
        completed = true;
        return;
    }

    if (auto err = mgr->add_request(request.easy.get()); !curl::ok(err)) {
        request.fail(Error::from_curl(curl::to_easy_error(err)));
        completed = true;
        return;
    }

    registered = true;
    mgr->drive_timeout_arming(inflight_request_opaque(shared_from_this()));
}

struct RequestAwaiter : uv::AwaitOp<RequestAwaiter> {
    using promise_t = Task<Response, Error>::promise_type;
    using result_type = Outcome<Response, Error, Cancellation>;

    InflightRequestRef state;

    RequestAwaiter(Manager &manager, InflightRequestRef request_state) : state(std::move(request_state)) {
        state->mgr = &manager;
        state->awaiter = this;
    }

    ~RequestAwaiter() {
        if (!state) {
            return;
        }

        state->detach_from_multi();
        state->release_request();
        state->awaiter = nullptr;
        state->mgr = nullptr;
    }

    static void on_cancel(IoOp *op) {
        uv::AwaitOp<RequestAwaiter>::complete_cancel(op, [](RequestAwaiter &self) {
            self.state->detach_from_multi();
            self.state->release_request();
            self.state->completed = true;
        });
    }

    void start() noexcept { state->start_transfer(); }

    bool await_ready() const noexcept { return state->completed; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        return this->attach(waiting.promise(), loc);
    }

    result_type await_resume() noexcept {
        state->detach_from_multi();

        if (static_cast<AsyncNode &>(*this).state == AsyncNode::CANCELLED) {
            return result_type(outcome_cancel(Cancellation("http request cancelled")));
        }

        if (state->request_released) {
            return result_type(outcome_error(Error::invalid_request("request state already released")));
        }

        return state->request.finish();
    }
};

void InflightRequestState::complete(Error err, bool resume) noexcept {
    if (completed) {
        return;
    }

    completed = true;
    registered = false;
    if (!request_released) {
        request.result = std::move(err);
    }

    // Streaming consumers wait on the conduit Events rather than an IoOp;
    // Event::set defers resumption to the loop's check phase, so signaling
    // here is safe from any curl/uv callback context.
    if (!request_released && request.conduit) {
        request.conduit->body_done = true;
        request.conduit->headers_ready.set();
        request.conduit->readable.set();
    }

    auto *waiting = awaiter;
    if (resume && waiting) {
        waiting->complete();
    }
}

InflightRequestRef make_inflight_request_state(http::Request request) noexcept {
    return std::make_shared<InflightRequestState>(std::move(request));
}

void *inflight_request_opaque(const InflightRequestRef &request) noexcept { return request.get(); }

InflightRequestRef retain_inflight_request(void *opaque) noexcept {
    auto *request = static_cast<InflightRequestState *>(opaque);
    if (!request) {
        return {};
    }

    return request->weak_from_this().lock();
}

void mark_inflight_request_removed(const InflightRequestRef &request) noexcept {
    if (request) {
        request->registered = false;
    }
}

void complete_inflight_request(const InflightRequestRef &request, curl::EasyError result, bool resume_inline) noexcept {
    if (!request) {
        return;
    }

    request->complete(result, !resume_inline);
}

Task<Response, Error> execute_request(http::Request request, EventLoop &loop) {
    // libcurl callbacks keep `userdata = this`, so the prepared request must stay at a stable
    // address for the rest of its lifetime.
    auto state = make_inflight_request_state(std::move(request));
    if (!state->request.prepare()) {
        co_await fail(std::move(state->request.result));
    }

    auto manager = Manager::try_for_loop(loop);
    if (!manager) {
        co_await fail(std::move(manager.error()));
    }

    RequestAwaiter awaiter(manager->get(), std::move(state));
    awaiter.start();
    auto result = co_await awaiter;

    if (result.is_cancelled()) {
        co_await cancel();
    }

    if (result.has_error()) {
        co_await fail(std::move(result).error());
    }

    co_return std::move(*result);
}

}} // namespace lighter::http::detail
