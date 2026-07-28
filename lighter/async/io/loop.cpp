#include "loop.h"

#include <atomic>
#include <cassert>
#include <deque>
#include <mutex>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/detail/libuv_helper.h>
#include <lighter/utils/functional.h>
#include <lighter/async/io/watcher.h>
#include <lighter/async/runtime/node.h>

namespace lighter {

struct Relay::Self {
    uv_async_t async = {};
    std::mutex mutex;
    std::vector<Function<void()>> queue;
    std::atomic<i32> count{0};
};

struct EventLoop::Self : Relay::Self {
    uv_loop_t loop = {};
    uv_idle_t idle = {};
    uv_check_t check = {};
    bool idle_running = false;
    bool check_running = false;
    std::deque<AsyncNode *> tasks;
    std::deque<AsyncNode *> deferred;
    /// Ops suspended via yield(). New ops land in `yields_staged`; each()
    /// promotes the staged batch to `yields_ready` and completes the batch
    /// promoted by the previous each(). The two-step promotion guarantees an
    /// op never resumes in the iteration that enqueued it, no matter which
    /// callback phase (Timer, Idle, poll, Check) it was enqueued from.
    std::deque<IoOp *> yields_staged;
    std::deque<IoOp *> yields_ready;
    std::vector<Function<void()>> destroy_callbacks;
};

static void on_relay(uv_async_t *handle) {
    auto *self = static_cast<EventLoop::Self *>(handle->data);
    std::vector<Function<void()>> batch;
    {
        std::lock_guard lock(self->mutex);
        batch = std::move(self->queue);
    }
    for (auto &cb : batch) {
        cb();
    }

    // Release the loop hold only when no Relay is alive AND the queue is
    // still empty. A producer may send() and destroy its Relay while the
    // batch above is draining; the re-armed async wakeup alone would not
    // keep uv_run alive once the handle is unreffed, and the refilled queue
    // would be dropped. count == 0 (acquire) pairs with the release
    // decrement in ~Relay: it guarantees every send() on the destroyed
    // relays is already visible in the queue, so checking both under the
    // Mutex is race-free.
    std::lock_guard lock(self->mutex);
    if (self->count.load(std::memory_order_acquire) == 0 && self->queue.empty()) {
        uv::unref(*handle);
    }
}

Relay::Relay(Relay::Self *p) noexcept : self(p) {}

Relay::Relay(Relay &&other) noexcept : self(std::exchange(other.self, nullptr)) {}

Relay &Relay::operator=(Relay &&other) noexcept {
    if (this != &other) {
        auto *old = std::exchange(self, std::exchange(other.self, nullptr));
        if (old) {
            old->count.fetch_sub(1, std::memory_order_release);
            uv::async_send(old->async);
        }
    }
    return *this;
}

Relay::~Relay() {
    if (self) {
        self->count.fetch_sub(1, std::memory_order_release);
        uv::async_send(self->async);
    }
}

void Relay::send(Function<void()> callback) {
    if (!self) {
        return;
    }
    std::lock_guard lock(self->mutex);
    self->queue.push_back(std::move(callback));
    uv::async_send(self->async);
}

Relay EventLoop::create_relay() {
    if (self->count.fetch_add(1, std::memory_order_relaxed) == 0) {
        uv::ref(self->async);
    }
    return Relay(self.get());
}

static thread_local EventLoop *current_loop = nullptr;

EventLoop &EventLoop::current() {
    assert(current_loop && "EventLoop::current() called outside a running loop");
    return *current_loop;
}

bool EventLoop::has_current() noexcept { return current_loop != nullptr; }

void each(uv_idle_t *idle) {
    auto self = static_cast<EventLoop::Self *>(idle->data);

    if (self->idle_running && self->tasks.empty() && self->yields_staged.empty() && self->yields_ready.empty()) {
        self->idle_running = false;
        uv::idle_stop(*idle);
        return;
    }

    // Promote the staged yields and snapshot the Task batch up front:
    // anything produced by the resumes below belongs to a later iteration.
    auto yielded = std::move(self->yields_ready);
    self->yields_ready = std::move(self->yields_staged);
    auto all = std::move(self->tasks);

    for (auto &task : all) {
        task->resume();
    }

    // Complete the previously promoted yields after this iteration's
    // scheduled tasks - yield() resumes only once everything queued before
    // it has run. A cancelled yield op is never dequeued early; its
    // completion here reports the CANCELLED state, so no entry in this
    // batch can dangle.
    for (auto *op : yielded) {
        op->complete();
    }
}

void EventLoop::schedule(AsyncNode &frame, std::source_location loc) {
    assert(self && "schedule: no current Event loop in this thread");

    if (frame.state == AsyncNode::PENDING) {
        frame.state = AsyncNode::RUNNING;
    } else if (frame.state == AsyncNode::FINISHED || frame.state == AsyncNode::RUNNING) {
        std::abort();
    }

    frame.location = loc;
    auto &loop = *this;
    if (!loop->idle_running && loop->tasks.empty()) {
        loop->idle_running = true;
        uv::idle_start(loop->idle, each);
    }
    loop->tasks.push_back(&frame);
}

void EventLoop::start(AsyncNode &frame, std::source_location loc) {
    assert(self && "start: no Event loop in this thread");
    assert(frame.state == AsyncNode::PENDING && "start requires a pending Task");

    frame.state = AsyncNode::RUNNING;
    frame.location = loc;

    auto *previous = current_loop;
    current_loop = this;
    frame.resume();
    current_loop = previous;
}

static void drain_deferred_queue(EventLoop::Self *self) {
    while (!self->deferred.empty()) {
        auto batch = std::move(self->deferred);
        for (auto *node : batch) {
            node->resume();
        }
    }
}

static void on_check(uv_check_t *handle) {
    auto *self = static_cast<EventLoop::Self *>(handle->data);
    drain_deferred_queue(self);
    if (self->check_running) {
        self->check_running = false;
        uv::check_stop(*handle);
        uv::unref(*handle);
    }
}

void EventLoop::defer_resume(AsyncNode &node) {
    self->deferred.push_back(&node);
    if (!self->check_running) {
        self->check_running = true;
        uv::ref(self->check);
        uv::check_start(self->check, on_check);
    }
}

void EventLoop::drain_deferred() { drain_deferred_queue(self.get()); }

void EventLoop::on_destroy(Function<void()> callback) { self->destroy_callbacks.push_back(std::move(callback)); }

YieldAwaiter::YieldAwaiter(EventLoop &loop) noexcept : loop(&loop) {
    // Cancellation needs no action: the op is intentionally left queued, and
    // the queued completion in each() delivers the CANCELLED Outcome on the
    // next iteration (structured completion). Never dequeuing on cancel is
    // also what keeps the each() batch free of dangling pointers.
    action = +[](IoOp *) {};
}

std::coroutine_handle<> YieldAwaiter::suspend(AsyncNode &parent_node, std::source_location loc) noexcept {
    auto *self = loop->operator->();

    // Enqueue before attach: when the parent is already cancelled, attach's
    // Cancellation checkpoint cancels this op in place, and the queued
    // completion still resolves it in a later iteration.
    self->yields_staged.push_back(this);
    if (!self->idle_running) {
        self->idle_running = true;
        uv::idle_start(self->idle, each);
    }

    return attach(parent_node, loc);
}

EventLoop::EventLoop() : self(new Self()) {
    auto &loop = self->loop;
    if (auto err = uv::loop_init(loop)) {
        abort();
    }

    auto &idle = self->idle;
    uv::idle_init(loop, idle);
    idle.data = self.get();

    auto &check = self->check;
    uv::check_init(loop, check);
    check.data = self.get();
    uv::unref(check);

    auto &async = self->async;
    uv::async_init(loop, async, on_relay);
    async.data = self.get();
    uv::unref(async);
}

EventLoop::~EventLoop() {
    constexpr static auto cleanup = +[](uv_handle_t *h, void *arg) {
        auto *self = static_cast<EventLoop::Self *>(arg);
        if (!uv::is_closing(*h)) {
            auto *idle = uv::as_handle(self->idle);
            auto *check = uv::as_handle(self->check);
            auto *async = uv::as_handle(self->async);
            if (h == idle || h == check || h == async) {
                uv::close(*h, nullptr);
                return;
            }

            uv::close(*h, [](uv_handle_t *handle) { uv::LoopCloseFallback::mark(handle); });
        }
    };

    assert(self->count.load(std::memory_order_acquire) == 0 && "EventLoop destroyed with live relays");

    {
        std::lock_guard lock(self->mutex);
        self->queue.clear();
    }

    auto callbacks = std::move(self->destroy_callbacks);
    for (auto &callback : callbacks) {
        callback();
    }

    auto &loop = self->loop;
    auto close_err = uv::loop_close(loop);
    if (close_err.value() == UV_EBUSY) {
        uv::walk(loop, cleanup, self.get());

        // Run Event loop to trigger all close callbacks.
        while ((close_err = uv::loop_close(loop)).value() == UV_EBUSY) {
            uv::run(loop, UV_RUN_ONCE);
        }
    }
}

EventLoop::operator uv_loop_t &() noexcept { return self->loop; }

EventLoop::operator const uv_loop_t &() const noexcept { return self->loop; }

i32 EventLoop::run() {
    auto previous = current_loop;
    current_loop = this;
    const i32 result = uv::run(self->loop, UV_RUN_DEFAULT);
    current_loop = previous;
    return result;
}

void EventLoop::stop() { uv::stop(self->loop); }

} // namespace lighter
