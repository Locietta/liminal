#pragma once

#include <memory>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

#include <uv.h>
#include <lighter/types.hpp>
#include <lighter/utils/functional.h>

namespace lighter {

class AsyncNode;

template <typename T, typename E, typename C> class Task;

/// A thread-safe Relay for posting callbacks to an Event loop.
///
/// Creating a Relay keeps the Event loop alive until the Relay is
/// destroyed.
///
/// A default-constructed or moved-from Relay is inert: send() is a
/// safe no-op, and destruction has no effect.
///
/// Usage (one-shot):
///   auto Relay = loop.create_relay();
///   some_system_async_api([Relay = std::move(Relay)](auto result) mutable {
///       Relay.send([result] { /* runs on loop thread */ });
///   });
///
/// Usage (recurring):
///   Relay notify = loop.create_relay();
///   // from any thread, repeatedly:
///   notify.send([&] { drain_buffer(); });
///   // destroy the Relay (or let it go out of scope) to release the loop hold.
///
/// Ownership:
///   The Relay object is single-owner and non-copyable. send() is
///   thread-safe with respect to other send() calls, but the Relay
///   must not be destroyed while any send() call is in progress.
///
/// Lifetime:
///   The EventLoop must outlive all relays created from it. Using a
///   Relay after its EventLoop is destroyed is undefined behavior.
///
/// Thread safety:
///   - Construction (create_relay) is NOT thread-safe; call it on the
///     loop thread before handing the Relay off.
///   - send() is thread-safe and can be called multiple times.
///   - Destroying the Relay releases the loop hold. PENDING callbacks
///     that were already enqueued are still delivered, unless the
///     EventLoop itself is being destroyed (which clears the queue).
class Relay {
public:
    Relay() noexcept = default;

    Relay(const Relay &) = delete;
    Relay &operator=(const Relay &) = delete;

    Relay(Relay &&other) noexcept;
    Relay &operator=(Relay &&other) noexcept;

    ~Relay();

    /// Posts a callback to the Event loop.
    ///
    /// Thread-safe. Can be called multiple times. Callbacks are executed
    /// on the loop thread in FIFO enqueue order. Concurrent producers
    /// are serialized by a Mutex, so cross-thread ordering follows
    /// Mutex acquisition order.
    void send(Function<void()> callback);

    /// Opaque implementation detail. Defined in loop.cpp.
    struct Self;

private:
    friend class EventLoop;

    explicit Relay(Self *p) noexcept;

    Self *self = nullptr;
};

/// Runs an Event loop backed by libuv.
///
/// All async operations (tasks, timers, I/O) require an EventLoop.
/// Each thread may have at most one active loop (thread-local).
/// Use EventLoop::current() inside a running loop to get a reference.
class EventLoop {
public:
    EventLoop();

    ~EventLoop();

    /// Returns the Event loop running on the current thread.
    static EventLoop &current();

    /// Returns true if a loop is running on the current thread.
    static bool has_current() noexcept;

    /// Opaque implementation detail. Defined in loop.cpp.
    struct Self;

    /// Internal accessor for the implementation struct.
    Self *operator->() { return self.get(); }

    friend class AsyncNode;

public:
    operator uv_loop_t &() noexcept;

    operator const uv_loop_t &() const noexcept;

    i32 run();

    void stop();

    /// Creates a Relay that keeps this Event loop alive until destroyed.
    ///
    /// NOT thread-safe: must be called on the loop thread. The returned Relay
    /// object can then be moved to another thread or captured in a system API
    /// callback, where Relay::send() can be called thread-safely.
    Relay create_relay();

    /// Registers a callback to run during EventLoop destruction, before the
    /// underlying uv loop is closed.
    ///
    /// NOT thread-safe: intended for loop-affine subsystems that need to
    /// release handles tied to this loop.
    void on_destroy(Function<void()> callback);

    /// Schedules a Task for execution on this Event loop.
    /// If the Task is passed by rvalue (temporary), the loop takes ownership
    /// (sets root=true). The Task will be destroyed after it completes.
    template <typename TaskT> void schedule(TaskT &&task, std::source_location location = std::source_location::current()) {
        auto &promise = task.h.promise();
        if constexpr (std::is_rvalue_reference_v<TaskT &&>) {
            promise.root = true;
            task.release();
        }

        schedule(static_cast<AsyncNode &>(promise), location);
    }

    /// Queues a node for deferred resumption.
    ///
    /// Unlike schedule(), this does not Check or modify the node's state.
    /// Used by sync primitives to defer waiter resumes instead of resuming
    /// inline (which would cause reentrancy).
    void defer_resume(AsyncNode &node);

    /// Drains all deferred resumes. The runtime calls this after the outermost
    /// coroutine resume returns; a Check handle is kept as a fallback so queued
    /// resumes still run before the next loop iteration.
    void drain_deferred();

private:
    void schedule(AsyncNode &frame, std::source_location location);

    std::unique_ptr<Self> self;
};

/// Convenience: creates a loop, schedules all tasks, runs to completion,
/// and returns a tuple of their values (via task::value()).
template <typename... Tasks> auto run(Tasks &&...tasks) {
    EventLoop loop;
    (loop.schedule(tasks), ...);
    loop.run();
    return std::tuple(std::move(tasks.value())...);
}

} // namespace lighter
