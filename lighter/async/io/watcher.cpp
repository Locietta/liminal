#include "watcher.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <type_traits>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

struct Timer::Self : uv::handle<Timer::Self, uv_timer_t> {
    uv_timer_t handle{};
    IoOp *waiter = nullptr;
    i32 pending = 0;
};

struct Idle::Self : uv::handle<Idle::Self, uv_idle_t> {
    uv_idle_t handle{};
    IoOp *waiter = nullptr;
    i32 pending = 0;
};

struct Prepare::Self : uv::handle<Prepare::Self, uv_prepare_t> {
    uv_prepare_t handle{};
    IoOp *waiter = nullptr;
    i32 pending = 0;
};

struct Check::Self : uv::handle<Check::Self, uv_check_t> {
    uv_check_t handle{};
    IoOp *waiter = nullptr;
    i32 pending = 0;
};

namespace {

template <typename SelfT, typename HandleT>
struct BasicTickAwait : uv::AwaitOp<BasicTickAwait<SelfT, HandleT>> {
    using await_base = uv::AwaitOp<BasicTickAwait<SelfT, HandleT>>;
    using promise_t = Task<void, Error>::promise_type;

    // Watcher self that owns waiter/pending counters.
    SelfT *self;

    explicit BasicTickAwait(SelfT *watcher) : self(watcher) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                if constexpr (std::is_same_v<HandleT, uv_timer_t>) {
                    uv::timer_stop(aw.self->handle);
                } else if constexpr (std::is_same_v<HandleT, uv_idle_t>) {
                    uv::idle_stop(aw.self->handle);
                } else if constexpr (std::is_same_v<HandleT, uv_prepare_t>) {
                    uv::prepare_stop(aw.self->handle);
                } else if constexpr (std::is_same_v<HandleT, uv_check_t>) {
                    uv::check_stop(aw.self->handle);
                }
                aw.self->waiter = nullptr;
            }
        });
    }

    static void on_fire(HandleT *handle) {
        auto *watcher = static_cast<SelfT *>(handle->data);
        contract_assert(watcher != nullptr);

        if (watcher->waiter) {
            auto w = watcher->waiter;
            watcher->waiter = nullptr;
            w->complete();
        } else {
            watcher->pending += 1;
        }
    }

    bool await_ready() const noexcept { return self && self->pending > 0; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }
        self->waiter = this;
        return this->attach(waiting.promise(), loc);
    }

    void await_resume() noexcept {
        if (self && self->pending > 0) {
            self->pending -= 1;
        }

        if (self) {
            self->waiter = nullptr;
        }
    }
};

using timer_await = BasicTickAwait<Timer::Self, uv_timer_t>;
using idle_await = BasicTickAwait<Idle::Self, uv_idle_t>;
using prepare_await = BasicTickAwait<Prepare::Self, uv_prepare_t>;
using check_await = BasicTickAwait<Check::Self, uv_check_t>;

} // namespace

#define LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(WatcherType)                               \
    WatcherType::WatcherType() noexcept = default;                                        \
    WatcherType::WatcherType(UniqueHandle<Self> self) noexcept : self(std::move(self)) {} \
    WatcherType::~WatcherType() = default;                                                \
    WatcherType::WatcherType(WatcherType &&other) noexcept = default;                     \
    WatcherType &WatcherType::operator=(WatcherType &&other) noexcept = default;          \
    WatcherType::Self *WatcherType::operator->() noexcept { return self.get(); }

LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Timer)
LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Idle)
LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Prepare)
LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Check)

#undef LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS

Timer Timer::create(EventLoop &loop) {
    auto self = Self::make();
    auto &handle = self->handle;
    uv::timer_init(loop, handle);

    return Timer(std::move(self));
}

void Timer::start(std::chrono::milliseconds timeout, std::chrono::milliseconds repeat) {
    if (!self) {
        return;
    }

    auto &handle = self->handle;
    uv::timer_start(
        handle, [](uv_timer_t *h) { timer_await::on_fire(h); }, static_cast<u64>(timeout.count()), static_cast<u64>(repeat.count()));
}

void Timer::stop() {
    if (!self) {
        return;
    }

    uv::timer_stop(self->handle);
}

Task<void, Error> Timer::wait() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->pending > 0) {
        self->pending -= 1;
        co_return;
    }

    // Reported rather than asserted: with NDEBUG the old assert vanished and a
    // second waiter resumed immediately, indistinguishable from the Timer
    // having fired.
    if (self->waiter != nullptr) {
        co_await fail(Error::k_connection_already_in_progress);
    }

    co_await timer_await{self.get()};
}

#define LIGHTER_DEFINE_TICK_WATCHER_METHODS(WatcherType, HandleType, AwaiterType, INIT_FN, START_FN, STOP_FN) \
    WatcherType WatcherType::create(EventLoop &loop) {                                                        \
        auto self = Self::make();                                                                             \
        auto &handle = self->handle;                                                                          \
        INIT_FN(loop, handle);                                                                                \
                                                                                                              \
        return WatcherType(std::move(self));                                                                  \
    }                                                                                                         \
                                                                                                              \
    void WatcherType::start() {                                                                               \
        if (!self) {                                                                                          \
            return;                                                                                           \
        }                                                                                                     \
                                                                                                              \
        auto &handle = self->handle;                                                                          \
        START_FN(handle, [](HandleType *h) { AwaiterType::on_fire(h); });                                     \
    }                                                                                                         \
                                                                                                              \
    void WatcherType::stop() {                                                                                \
        if (!self) {                                                                                          \
            return;                                                                                           \
        }                                                                                                     \
                                                                                                              \
        STOP_FN(self->handle);                                                                                \
    }                                                                                                         \
                                                                                                              \
    Task<void, Error> WatcherType::wait() {                                                                   \
        if (!self) {                                                                                          \
            co_await fail(Error::k_invalid_argument);                                                         \
        }                                                                                                     \
                                                                                                              \
        if (self->pending > 0) {                                                                              \
            self->pending -= 1;                                                                               \
            co_return;                                                                                        \
        }                                                                                                     \
                                                                                                              \
        /* Reported rather than asserted: with NDEBUG the assert vanished and */                              \
        /* a second waiter resumed as if the watcher had ticked. */                                           \
        if (self->waiter != nullptr) {                                                                        \
            co_await fail(Error::k_connection_already_in_progress);                                           \
        }                                                                                                     \
                                                                                                              \
        co_await AwaiterType{self.get()};                                                                     \
    }

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Idle, uv_idle_t, idle_await, uv::idle_init, uv::idle_start, uv::idle_stop)

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Prepare, uv_prepare_t, prepare_await, uv::prepare_init, uv::prepare_start, uv::prepare_stop)

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Check, uv_check_t, check_await, uv::check_init, uv::check_start, uv::check_stop)

#undef LIGHTER_DEFINE_TICK_WATCHER_METHODS

Task<> sleep(std::chrono::milliseconds timeout, EventLoop &loop) {
    auto t = Timer::create(loop);
    t.start(timeout, std::chrono::milliseconds{0});

    // The Timer is local and freshly created, so the only failures wait() can
    // report - no handle, or a second concurrent waiter - are both unreachable
    // here. Keeping sleep() as Task<> spares every caller an error channel it
    // could never observe.
    auto result = co_await t.wait();
    contract_assert(!result.has_error());
}

} // namespace lighter
