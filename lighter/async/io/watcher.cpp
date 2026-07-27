#include "watcher.h"

#include <algorithm>
#include <cassert>
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

struct Signal::Self : uv::handle<Signal::Self, uv_signal_t> {
    uv_signal_t handle{};
    IoOp *waiter = nullptr;
    Error *active = nullptr;
    i32 pending = 0;
};

/// One libuv watcher inside a SignalSet.
///
/// Each uv_signal_t needs its own stable address and its own handle->data, but
/// they all deliver into the set's single queue - so `data` points at this
/// Slot, which names both the owning set and which signal fired.
///
/// Slots are heap-allocated individually (rather than living in a vector) so
/// that the handle addresses stay put; uv_close() runs asynchronously and reads
/// handle->data long after the owner has moved on.
struct SignalSlot : uv::handle<SignalSlot, uv_signal_t> {
    uv_signal_t handle{};
    SignalSet::Self *owner = nullptr;
    SignalKind kind = SignalKind::INT;
};

/// Unlike the other wrappers, this owns no libuv handle of its own - the slots
/// do - so it is a plain heap object. Destroying it drops the slots, and each
/// slot's UniqueHandle defers its own delete until uv_close completes.
struct SignalSet::Self : uv::QueuedDelivery<Result<SignalKind>> {
    std::vector<UniqueHandle<SignalSlot>> slots;

    /// Merely watching for a signal is not work: the handles stay unreferenced
    /// so a program with nothing left to do can exit instead of parking forever
    /// on a Ctrl+C that may never come.
    ///
    /// An active wait() IS work, though. While one is suspended the handles are
    /// referenced, or uv_run would return with the waiter still parked and the
    /// signal never observed - a program whose only job is to await Ctrl+C would
    /// exit immediately.
    void ref_slots() noexcept {
        for (auto &slot : slots) {
            uv::ref(slot->handle);
        }
    }

    void unref_slots() noexcept {
        for (auto &slot : slots) {
            uv::unref(slot->handle);
        }
    }

    static void destroy(Self *self) noexcept { delete self; }
};

namespace {

template <typename SelfT, typename HandleT>
struct BasicTickAwait : uv::AwaitOp<BasicTickAwait<SelfT, HandleT>> {
    using await_base = uv::AwaitOp<BasicTickAwait<SelfT, HandleT>>;
    using promise_t = Task<>::promise_type;

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
        assert(watcher != nullptr && "on_fire requires watcher state in handle->data");

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

struct SignalAwait : uv::AwaitOp<SignalAwait> {
    using await_base = uv::AwaitOp<SignalAwait>;
    using promise_t = Task<void, Error>::promise_type;

    // Signal watcher self that owns waiter/active pointers.
    Signal::Self *self;
    // Result slot returned by await_resume().
    Error result{};

    explicit SignalAwait(Signal::Self *watcher) : self(watcher) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                uv::signal_stop(aw.self->handle);
                aw.self->waiter = nullptr;
                aw.self->active = nullptr;
            }
        });
    }

    static void on_fire(uv_signal_t *handle) {
        auto *watcher = static_cast<Signal::Self *>(handle->data);
        assert(watcher != nullptr && "on_fire requires watcher state in handle->data");

        if (watcher->waiter && watcher->active) {
            *watcher->active = {};

            auto w = watcher->waiter;
            watcher->waiter = nullptr;
            watcher->active = nullptr;

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
        self->active = &result;
        return this->attach(waiting.promise(), loc);
    }

    Error await_resume() noexcept {
        if (self && self->pending > 0) {
            self->pending -= 1;
        }

        if (self) {
            self->waiter = nullptr;
            self->active = nullptr;
        }
        return result;
    }
};

struct SignalSetAwait : uv::AwaitOp<SignalSetAwait> {
    using await_base = uv::AwaitOp<SignalSetAwait>;
    using promise_t = Task<SignalKind, Error>::promise_type;

    // Set state owning the shared delivery queue.
    SignalSet::Self *self;
    // Whether this wait should hold the Event loop open while suspended.
    bool keep_alive = true;
    // Result slot filled by deliver() before this operation completes.
    Result<SignalKind> result{outcome_error(Error::k_operation_aborted)};

    explicit SignalSetAwait(SignalSet::Self *set, bool keep_alive = true) : self(set), keep_alive(keep_alive) {}

    static void on_cancel(IoOp *op) {
        // Unlike the single-signal awaiter, cancelling one wait does NOT stop
        // the watchers: the set outlives any individual wait, and a cancelled
        // turn should not silently deafen the process to Ctrl+C.
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                aw.self->disarm();
                if (aw.keep_alive) {
                    aw.self->unref_slots();
                }
            }
        });
    }

    static void on_fire(uv_signal_t *handle) {
        auto *slot = static_cast<SignalSlot *>(handle->data);
        assert(slot != nullptr && "on_fire requires slot state in handle->data");
        assert(slot->owner != nullptr && "signal slot outlived its set");

        slot->owner->deliver(Result<SignalKind>(slot->kind));
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }
        self->arm(*this, result);
        // A caller's wait is real work, so hold the loop open for it. A
        // background wait deliberately does not, or a perpetual supervisor
        // would keep the process up forever.
        if (keep_alive) {
            self->ref_slots();
        }
        return this->attach(waiting.promise(), loc);
    }

    Result<SignalKind> await_resume() noexcept {
        if (self) {
            self->disarm();
            if (keep_alive) {
                self->unref_slots();
            }
        }
        return std::move(result);
    }
};

} // namespace

#define LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(WatcherType)                               \
    WatcherType::WatcherType() noexcept = default;                                        \
    WatcherType::WatcherType(UniqueHandle<Self> self) noexcept : self(std::move(self)) {} \
    WatcherType::~WatcherType() = default;                                                \
    WatcherType::WatcherType(WatcherType &&other) noexcept = default;                     \
    WatcherType &WatcherType::operator=(WatcherType &&other) noexcept = default;          \
    WatcherType::Self *WatcherType::operator->() noexcept { return self.get(); }

LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Timer)
LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(Signal)
LIGHTER_DEFINE_WATCHER_SPECIAL_MEMBERS(SignalSet)
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
    assert(timeout.count() >= 0 && "Timer::start timeout must be non-negative");
    assert(repeat.count() >= 0 && "Timer::start repeat must be non-negative");
    uv::timer_start(
        handle, [](uv_timer_t *h) { timer_await::on_fire(h); }, static_cast<u64>(timeout.count()), static_cast<u64>(repeat.count()));
}

void Timer::stop() {
    if (!self) {
        return;
    }

    uv::timer_stop(self->handle);
}

Task<> Timer::wait() {
    if (!self) {
        co_return;
    }

    if (self->pending > 0) {
        self->pending -= 1;
        co_return;
    }

    if (self->waiter != nullptr) {
        assert(false && "Timer::wait supports a single waiter at a time");
        co_return;
    }

    co_await timer_await{self.get()};
}

Result<Signal> Signal::create(EventLoop &loop) {
    auto self = Self::make();
    auto &handle = self->handle;
    if (auto err = uv::signal_init(loop, handle)) {
        return outcome_error(err);
    }

    return Signal(std::move(self));
}

Error Signal::start(i32 signum) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    auto &handle = self->handle;
    if (auto err = uv::signal_start(handle, [](uv_signal_t *h, i32) { SignalAwait::on_fire(h); }, signum); err) {
        return err;
    }

    return {};
}

Error Signal::stop() {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::signal_stop(self->handle)) {
        return err;
    }

    return {};
}

Task<void, Error> Signal::wait() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->pending > 0) {
        self->pending -= 1;
        co_return;
    }

    if (self->waiter != nullptr) {
        co_await fail(Error::k_connection_already_in_progress);
    }

    if (auto err = co_await SignalAwait{self.get()}) {
        co_await fail(std::move(err));
    }
}

i32 signal_number(SignalKind kind) noexcept {
    switch (kind) {
        case SignalKind::INT: return SIGINT;
        case SignalKind::TERM: return SIGTERM;

        // SIGHUP/SIGQUIT/SIGWINCH are absent from the MSVC/MinGW <csignal>, but
        // libuv synthesizes SIGHUP from CTRL_CLOSE_EVENT and accepts the POSIX
        // numbers, so spell them out rather than dropping the kind entirely.
        case SignalKind::HUP:
#ifdef SIGHUP
            return SIGHUP;
#else
            return 1;
#endif
        case SignalKind::QUIT:
#ifdef SIGQUIT
            return SIGQUIT;
#else
            return -1;
#endif
        case SignalKind::BREAK:
#ifdef SIGBREAK
            return SIGBREAK;
#else
            return -1;
#endif
        case SignalKind::WINCH:
            // uv/win.h defines SIGWINCH (28) where the CRT does not, so this
            // resolves on both platforms once <uv.h> is in scope.
#ifdef SIGWINCH
            return SIGWINCH;
#else
            return -1;
#endif
    }

    return -1;
}

bool signal_supported(SignalKind kind) noexcept {
    if (signal_number(kind) < 0) {
        return false;
    }

#ifdef _WIN32
    // Windows has no signals; libuv fabricates these. INT/BREAK/HUP come from
    // console control events, and WINCH is emulated from console resize
    // detection (which libuv documents as not always timely, and which only
    // notices readable-tty changes while the handle is in raw mode and being
    // read). Everything else - TERM included - would install a watcher that can
    // never fire.
    return kind == SignalKind::INT || kind == SignalKind::BREAK || kind == SignalKind::HUP || kind == SignalKind::WINCH;
#else
    return kind != SignalKind::BREAK;
#endif
}

Result<SignalSet> SignalSet::create(std::span<const SignalKind> kinds, EventLoop &loop) {
    if (kinds.empty()) {
        return outcome_error(Error::k_invalid_argument);
    }

    auto self = UniqueHandle<Self>(new Self());

    for (auto kind : kinds) {
        // Reject on supportedness, not just on the numeric mapping: TERM has a
        // perfectly good CRT number on Windows, yet libuv can never deliver it,
        // so accepting it would hand back a watcher that hangs forever instead
        // of the documented error. Callers that want a best-effort set should
        // filter with signal_supported() first - InterruptSource does.
        if (!signal_supported(kind)) {
            return outcome_error(Error::k_invalid_argument);
        }

        const i32 signum = signal_number(kind);

        // Watching the same signal twice would deliver it twice.
        const bool duplicate = std::ranges::any_of(self->slots, [kind](const auto &slot) { return slot->kind == kind; });
        if (duplicate) {
            continue;
        }

        auto slot = SignalSlot::make();
        slot->owner = self.get();
        slot->kind = kind;

        if (auto err = uv::signal_init(loop, slot->handle)) {
            return outcome_error(err);
        }

        if (auto err = uv::signal_start(slot->handle, [](uv_signal_t *h, i32) { SignalSetAwait::on_fire(h); }, signum)) {
            return outcome_error(err);
        }

        // Watching for a signal is not itself work: an unreferenced handle lets
        // the loop exit once the real tasks are done, instead of parking
        // forever on a Ctrl+C that may never come.
        uv::unref(slot->handle);

        self->slots.push_back(std::move(slot));
    }

    return SignalSet(std::move(self));
}

Result<SignalSet> SignalSet::create(std::initializer_list<SignalKind> kinds, EventLoop &loop) {
    return create(std::span<const SignalKind>(kinds.begin(), kinds.size()), loop);
}

namespace {

Task<SignalKind, Error> wait_impl(SignalSet::Self *self, bool keep_alive) {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->has_pending()) {
        co_return co_await or_fail(self->take_pending());
    }

    if (self->has_waiter()) {
        co_await fail(Error::k_connection_already_in_progress);
    }

    co_return co_await or_fail(co_await SignalSetAwait{self, keep_alive});
}

} // namespace

Task<SignalKind, Error> SignalSet::wait() { return wait_impl(self.get(), true); }

Task<SignalKind, Error> SignalSet::wait_background() { return wait_impl(self.get(), false); }

void SignalSet::hold_loop() noexcept {
    if (self) {
        self->ref_slots();
    }
}

void SignalSet::release_loop() noexcept {
    if (self) {
        self->unref_slots();
    }
}

Error SignalSet::stop() {
    if (!self) {
        return Error::k_invalid_argument;
    }

    for (auto &slot : self->slots) {
        if (auto err = uv::signal_stop(slot->handle)) {
            return err;
        }
    }

    return {};
}

#define LIGHTER_DEFINE_TICK_WATCHER_METHODS(WatcherType, HandleType, AwaiterType, INIT_FN, START_FN, STOP_FN, NameLiteral) \
    WatcherType WatcherType::create(EventLoop &loop) {                                                                     \
        auto self = Self::make();                                                                                          \
        auto &handle = self->handle;                                                                                       \
        INIT_FN(loop, handle);                                                                                             \
                                                                                                                           \
        return WatcherType(std::move(self));                                                                               \
    }                                                                                                                      \
                                                                                                                           \
    void WatcherType::start() {                                                                                            \
        if (!self) {                                                                                                       \
            return;                                                                                                        \
        }                                                                                                                  \
                                                                                                                           \
        auto &handle = self->handle;                                                                                       \
        START_FN(handle, [](HandleType *h) { AwaiterType::on_fire(h); });                                                  \
    }                                                                                                                      \
                                                                                                                           \
    void WatcherType::stop() {                                                                                             \
        if (!self) {                                                                                                       \
            return;                                                                                                        \
        }                                                                                                                  \
                                                                                                                           \
        STOP_FN(self->handle);                                                                                             \
    }                                                                                                                      \
                                                                                                                           \
    Task<> WatcherType::wait() {                                                                                           \
        if (!self) {                                                                                                       \
            co_return;                                                                                                     \
        }                                                                                                                  \
                                                                                                                           \
        if (self->pending > 0) {                                                                                           \
            self->pending -= 1;                                                                                            \
            co_return;                                                                                                     \
        }                                                                                                                  \
                                                                                                                           \
        if (self->waiter != nullptr) {                                                                                     \
            assert(false && NameLiteral "::wait supports a single waiter at a time");                                      \
            co_return;                                                                                                     \
        }                                                                                                                  \
                                                                                                                           \
        co_await AwaiterType{self.get()};                                                                                  \
    }

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Idle, uv_idle_t, idle_await, uv::idle_init, uv::idle_start, uv::idle_stop, "Idle")

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Prepare, uv_prepare_t, prepare_await, uv::prepare_init, uv::prepare_start, uv::prepare_stop, "Prepare")

LIGHTER_DEFINE_TICK_WATCHER_METHODS(Check, uv_check_t, check_await, uv::check_init, uv::check_start, uv::check_stop, "Check")

#undef LIGHTER_DEFINE_TICK_WATCHER_METHODS

Task<> sleep(std::chrono::milliseconds timeout, EventLoop &loop) {
    auto t = Timer::create(loop);
    t.start(timeout, std::chrono::milliseconds{0});
    co_await t.wait();
}

} // namespace lighter
