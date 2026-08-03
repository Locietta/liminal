#include "request.h"

#include <contracts>
#include <functional>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

namespace {

struct WorkOp : uv::AwaitOp<WorkOp> {
    using promise_t = Task<void, Error>::promise_type;

    // libuv request object; req.data points back to this Awaiter.
    uv_work_t req{};
    // User-supplied callback executed on libuv's worker thread.
    std::move_only_function<void()> fn;
    // Invoked on the loop thread when the awaiting Task is cancelled, so an
    // already-running fn can observe Cancellation and return early. Always
    // set; the hook-less overload passes a no-op.
    std::move_only_function<void()> cancel_hook;
    // Completion status consumed by await_resume().
    Error result;

    WorkOp(std::move_only_function<void()> fn, std::move_only_function<void()> cancel_hook)
        : fn(std::move(fn)), cancel_hook(std::move(cancel_hook)) {}

    static void on_cancel(IoOp *op) {
        auto *self = static_cast<WorkOp *>(op);
        // Dequeue first so the hook cannot indirectly free a pool thread that
        // would pick this work up before uv_cancel runs. If dequeuing fails
        // the work is running (or just finished); the hook tells it to return
        // early.
        if (uv::cancel(self->req)) {
            self->cancel_hook();
        }
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        return this->attach(waiting.promise(), loc);
    }

    Error await_resume() noexcept { return result; }
};

} // namespace

Task<void, Error> queue(std::move_only_function<void()> fn, std::move_only_function<void()> on_cancel, EventLoop &loop) {
    WorkOp op(std::move(fn), std::move(on_cancel));

    auto work_cb = [](uv_work_t *req) {
        auto *holder = static_cast<WorkOp *>(req->data);
        contract_assert(holder != nullptr);
        holder->fn();
    };

    auto after_cb = [](uv_work_t *req, i32 status) {
        auto *holder = static_cast<WorkOp *>(req->data);
        contract_assert(holder != nullptr);

        holder->mark_cancelled_if(status);
        holder->result = uv::status_to_error(status);
        holder->complete();
    };

    op.result.clear();
    op.req.data = &op;

    if (auto err = uv::queue_work(loop, op.req, work_cb, after_cb)) {
        co_await fail(err);
    }

    if (auto err = co_await op) {
        co_await fail(std::move(err));
    }
}

Task<void, Error> queue(std::move_only_function<void()> fn, EventLoop &loop) {
    return queue(std::move(fn), std::move_only_function<void()>([] {}), loop);
}

} // namespace lighter
