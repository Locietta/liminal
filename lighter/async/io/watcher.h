#pragma once

#include <chrono>
#include <initializer_list>
#include <span>

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

    /// Both durations must be non-negative; a negative one is a caller bug and
    /// panics rather than being cast into an effectively infinite wait.
    void start(std::chrono::milliseconds timeout, std::chrono::milliseconds repeat = {});

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

struct Signal {
    Signal() noexcept;

    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    Signal(Signal &&other) noexcept;
    Signal &operator=(Signal &&other) noexcept;

    ~Signal();

    struct Self;
    Self *operator->() noexcept;

    static Result<Signal> create(EventLoop &loop = EventLoop::current());

    Error start(i32 signum);

    Error stop();

    Task<void, Error> wait();

private:
    explicit Signal(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

/// Signals a console application can meaningfully observe, named so that call
/// sites do not have to spell raw signal numbers.
///
/// Platform support is uneven, and libuv only papers over part of it. On
/// Windows there are no real signals: libuv synthesizes these from console
/// control events, so a watcher for an unsupported signal is created happily
/// and then simply never fires.
///
///   INT   - Ctrl+C. Everywhere. Not delivered while the tty is in raw mode,
///           where the keypress is read as ordinary input instead.
///   TERM  - polite termination request. POSIX only; on Windows nothing
///           generates it, and Task Manager's "End task" calls
///           TerminateProcess, which no process can observe or refuse.
///   HUP   - terminal hangup on POSIX; the console window being closed on
///           Windows. On Windows the process is killed a few seconds later
///           regardless of what the handler does.
///   QUIT  - Ctrl+\ on POSIX. Never fires on Windows.
///   BREAK - Ctrl+Break. Windows only.
///   WINCH - terminal was resized. Emulated on Windows, where libuv documents
///           delivery as not always timely, and detects a readable terminal's
///           resize only while that handle is in raw mode and being read.
enum struct SignalKind : i32 {
    INT,
    TERM,
    HUP,
    QUIT,
    BREAK,
    WINCH,
};

/// Platform signal number for `kind`, or -1 where the platform has no such
/// signal. A -1 kind can never be watched; SignalSet::create rejects it.
i32 signal_number(SignalKind kind) noexcept;

/// True if this platform can actually deliver `kind`.
///
/// Stricter than "has a signal number": TERM has a perfectly good one on
/// Windows, yet nothing there can ever raise it, so a watcher would be created
/// successfully and then never fire. Use this to filter an ideal set down to
/// what the platform honours - InterruptSource does exactly that.
bool signal_supported(SignalKind kind) noexcept;

/// Watches several signals at once and reports which one arrived.
///
/// The single-signal `Signal` above cannot say what it observed - its callback
/// discards the signal number - so watching three signals means three objects
/// and three concurrent waits. SignalSet owns one uv_signal_t per signal and
/// funnels them into one queue.
///
/// Signals that arrive while nobody is waiting are queued, not dropped or
/// coalesced, so a burst is drained in arrival order by successive wait()
/// calls. This matters for the interactive case: a second Ctrl+C arriving while
/// the first is still being handled must remain visible.
///
/// Single-consumer: overlapping wait() calls fail with
/// k_connection_already_in_progress. Cancelling a pending wait() leaves the
/// watchers armed, so the set can be waited on again.
struct SignalSet {
    SignalSet() noexcept;

    SignalSet(const SignalSet &) = delete;
    SignalSet &operator=(const SignalSet &) = delete;

    SignalSet(SignalSet &&other) noexcept;
    SignalSet &operator=(SignalSet &&other) noexcept;

    ~SignalSet();

    struct Self;
    Self *operator->() noexcept;

    /// Starts watching every signal in `kinds`. Fails with k_invalid_argument
    /// if `kinds` is empty or names a signal this platform cannot deliver -
    /// see signal_supported(), which is stricter than "has a signal number":
    /// TERM has one on Windows but no console event can ever raise it.
    /// Duplicates are ignored.
    static Result<SignalSet> create(std::span<const SignalKind> kinds, EventLoop &loop = EventLoop::current());

    static Result<SignalSet> create(std::initializer_list<SignalKind> kinds, EventLoop &loop = EventLoop::current());

    /// Resolves with the signal that fired, draining any already queued.
    ///
    /// A pending wait keeps the Event loop alive: awaiting a signal is the
    /// program's work for as long as it lasts, so the loop must not exit and
    /// leave the waiter suspended forever.
    Task<SignalKind, Error> wait();

    /// wait() that does NOT keep the Event loop alive.
    ///
    /// For a supervisor that watches signals for the whole life of the process
    /// but should never be the reason it stays up - the loop stays free to exit
    /// once the real work is done, abandoning this wait. InterruptSource's pump
    /// is the motivating case; prefer wait() for anything a caller is actively
    /// blocking on.
    Task<SignalKind, Error> wait_background();

    /// Holds the Event loop open until released, so a caller blocked on
    /// something other than wait() - a queue fed by a background wait, say -
    /// is not left suspended by a loop that decided it had nothing to do.
    ///
    /// Balanced calls; release exactly once per hold. Holds nest: a libuv
    /// handle reference is a boolean rather than a counter, so these are
    /// counted and only the outermost pair touches libuv.
    void hold_loop() noexcept;
    void release_loop() noexcept;

    /// True while at least one hold is outstanding, i.e. this set is currently
    /// keeping the Event loop alive. Exposed for tests.
    bool holding_loop() const noexcept;

    /// Stops every watcher. Queued signals stay readable through wait().
    Error stop();

private:
    explicit SignalSet(UniqueHandle<Self> self) noexcept;

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

Task<> sleep(std::chrono::milliseconds timeout, EventLoop &loop = EventLoop::current());

inline Task<> sleep(i32 ms, EventLoop &loop = EventLoop::current()) { return sleep(std::chrono::milliseconds{ms}, loop); }

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
