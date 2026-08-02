#pragma once

#include <chrono>

#include <lighter/types.hpp>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

struct EventLoop;

struct Timer {
    Timer() noexcept;

    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;

    Timer(Timer &&other) noexcept;
    Timer &operator=(Timer &&other) noexcept;

    ~Timer();

    struct Self;
    Self *operator->() noexcept;

    static Timer create(EventLoop &loop = EventLoop::current());

    void start(std::chrono::milliseconds timeout, std::chrono::milliseconds repeat = {}) pre(timeout.count() >= 0 && repeat.count() >= 0);

    void stop();

    /// Waits for the next tick, consuming one already counted if the Timer
    /// fired while nobody was waiting.
    ///
    /// Single-consumer: a second concurrent wait fails with
    /// k_connection_already_in_progress rather than resuming as if the Timer
    /// had fired.
    Task<void, Error> wait();

private:
    explicit Timer(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

struct Idle {
    Idle() noexcept;

    Idle(const Idle &) = delete;
    Idle &operator=(const Idle &) = delete;

    Idle(Idle &&other) noexcept;
    Idle &operator=(Idle &&other) noexcept;

    ~Idle();

    struct Self;
    Self *operator->() noexcept;

    static Idle create(EventLoop &loop = EventLoop::current());

    void start();

    void stop();

    /// Single-consumer: a second concurrent wait fails with
    /// k_connection_already_in_progress rather than resuming spuriously.
    Task<void, Error> wait();

private:
    explicit Idle(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

struct Prepare {
    Prepare() noexcept;

    Prepare(const Prepare &) = delete;
    Prepare &operator=(const Prepare &) = delete;

    Prepare(Prepare &&other) noexcept;
    Prepare &operator=(Prepare &&other) noexcept;

    ~Prepare();

    struct Self;
    Self *operator->() noexcept;

    static Prepare create(EventLoop &loop = EventLoop::current());

    void start();

    void stop();

    /// Single-consumer: a second concurrent wait fails with
    /// k_connection_already_in_progress rather than resuming spuriously.
    Task<void, Error> wait();

private:
    explicit Prepare(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

struct Check {
    Check() noexcept;

    Check(const Check &) = delete;
    Check &operator=(const Check &) = delete;

    Check(Check &&other) noexcept;
    Check &operator=(Check &&other) noexcept;

    ~Check();

    struct Self;
    Self *operator->() noexcept;

    static Check create(EventLoop &loop = EventLoop::current());

    void start();

    void stop();

    /// Single-consumer: a second concurrent wait fails with
    /// k_connection_already_in_progress rather than resuming spuriously.
    Task<void, Error> wait();

private:
    explicit Check(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

Task<> sleep(std::chrono::milliseconds timeout, EventLoop &loop = EventLoop::current()) pre(timeout.count() >= 0);

inline Task<> sleep(i32 ms, EventLoop &loop = EventLoop::current()) pre(ms >= 0) { return sleep(std::chrono::milliseconds{ms}, loop); }

/// Awaitable returned by yield(): suspends and resumes no earlier than the
/// next Event-loop iteration, strictly after every callback, deferred resume
/// and scheduled Task that existed when it was enqueued - regardless of
/// which callback phase (Timer, Idle, poll, Check) performed the enqueue.
///
/// This is the primitive for "let the current cascade settle, then decide"
/// patterns (debounced Cancellation, coalesced re-checks). Unlike sleep(0) it
/// allocates no Timer and does not depend on libuv Timer-phase ordering, and
/// unlike the internal deferred-resume queue it never resumes within the
/// current drain cycle.
struct YieldAwaiter : IoOp {
    explicit YieldAwaiter(EventLoop &loop) noexcept;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h,
                                          std::source_location location = std::source_location::current()) noexcept {
        return suspend(h.promise(), location);
    }

    void await_resume() const noexcept {}

private:
    /// Enqueues on the loop's yield queue, then attaches. Defined in loop.cpp.
    std::coroutine_handle<> suspend(AsyncNode &parent_node, std::source_location loc) noexcept;

    EventLoop *loop = nullptr;
};

/// Suspends until the next Event-loop iteration.
inline YieldAwaiter yield(EventLoop &loop = EventLoop::current()) { return YieldAwaiter(loop); }

} // namespace lighter
