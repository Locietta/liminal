#pragma once

#include <cassert>
#include <cstddef>
#include <source_location>

#include <lighter/types.hpp>
#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/task.h>

namespace lighter {

/// Shared base for synchronization resources. These are not awaitable runtime
/// nodes; WaitNode sub-objects bridge tasks into the wait queue.
class SyncPrimitive {
public:
    enum class Kind : u8 {
        MUTEX,
        EVENT,
        SEMAPHORE,
        CONDITION_VARIABLE,
    };

    friend class AsyncNode;

    explicit SyncPrimitive(Kind k) : kind(k) {}

    const Kind kind;

    std::source_location location;

    /// Appends a waiter to the end of the wait queue.
    void insert(WaitNode *link);

    /// Removes a waiter from the wait queue.
    void remove(WaitNode *link);

    const WaitNode *get_head() const noexcept { return head; }

protected:
    bool has_waiters() const noexcept { return head != nullptr; }

    WaitNode *pop_waiter() noexcept {
        auto *link = head;
        if (link) {
            remove(link);
        }
        return link;
    }

    bool resume_waiter(WaitNode &link) noexcept;

    bool cancel_waiter(WaitNode &link) noexcept;

    /// Pops and processes every queued waiter.
    ///
    /// resume_waiter/cancel_waiter never run user code - they only tag the
    /// waiter and queue its awaiting Task on the Event loop - so no waiter
    /// can be enqueued or removed re-entrantly while this loop runs: popping
    /// until the queue is empty is a stable snapshot of the callers.
    template <typename Fn> void drain_waiters(Fn &&fn) {
        while (auto *waiter = pop_waiter()) {
            fn(*waiter);
        }
    }

private:
    /// Head and tail of the intrusive doubly-linked waiter queue.
    WaitNode *head = nullptr;
    WaitNode *tail = nullptr;
};

class Mutex : public SyncPrimitive {
public:
    Mutex(std::source_location location = std::source_location::current()) : SyncPrimitive(SyncPrimitive::Kind::MUTEX) {
        this->location = location;
    }

    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    struct LockAwaiter : WaitNode {
        explicit LockAwaiter(Mutex &owner) : WaitNode(AsyncNode::NodeKind::MUTEX_WAITER), owner(&owner) {
            abandon_context = &owner;
            abandon = &abandon_grant;
        }

        bool await_ready() noexcept { return owner->try_lock(); }

        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> h, std::source_location location = std::source_location::current()) noexcept {
            owner->insert(this);
            return attach(h.promise(), location);
        }

        void await_resume() noexcept {
            abandon_context = nullptr;
            abandon = nullptr;
        }

    private:
        static void abandon_grant(void *context) noexcept {
            if (auto *owner = static_cast<Mutex *>(context)) {
                owner->unlock();
            }
        }

        Mutex *owner = nullptr;
    };

    LockAwaiter lock() noexcept { return LockAwaiter(*this); }

    bool try_lock() noexcept {
        if (locked) {
            return false;
        }
        locked = true;
        return true;
    }

    void unlock() noexcept {
        assert(locked && "Mutex::unlock without lock");
        while (auto *waiter = pop_waiter()) {
            if (resume_waiter(*waiter)) {
                return;
            }
        }
        locked = false;
    }

private:
    bool locked = false;
};

class Semaphore : public SyncPrimitive {
public:
    explicit Semaphore(isize initial = 0, std::source_location location = std::source_location::current())
        : SyncPrimitive(SyncPrimitive::Kind::SEMAPHORE) {
        assert(initial >= 0 && "Semaphore initial count must be non-negative");
        this->location = location;
        count = initial;
    }

    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;

    struct AcquireAwaiter : WaitNode {
        /// Reuses EventWaiter kind - all WaitNode subtypes share identical
        /// cancel/attach/finalize logic, so a dedicated
        /// SemaphoreWaiter kind is unnecessary.
        explicit AcquireAwaiter(Semaphore &owner) : WaitNode(AsyncNode::NodeKind::EVENT_WAITER), owner(&owner) {
            abandon_context = &owner;
            abandon = &abandon_grant;
        }

        bool await_ready() noexcept { return owner->try_acquire(); }

        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> h, std::source_location location = std::source_location::current()) noexcept {
            owner->insert(this);
            return attach(h.promise(), location);
        }

        void await_resume() noexcept {
            abandon_context = nullptr;
            abandon = nullptr;
        }

    private:
        static void abandon_grant(void *context) noexcept {
            if (auto *owner = static_cast<Semaphore *>(context)) {
                owner->release();
            }
        }

        Semaphore *owner = nullptr;
    };

    AcquireAwaiter acquire() noexcept { return AcquireAwaiter(*this); }

    bool try_acquire() noexcept {
        if (count <= 0) {
            return false;
        }
        count -= 1;
        return true;
    }

    void release(isize n = 1) {
        assert(n >= 0 && "Semaphore::release count must be non-negative");
        for (isize i = 0; i < n; ++i) {
            bool transferred = false;
            if (has_waiters()) {
                while (auto *waiter = pop_waiter()) {
                    if (resume_waiter(*waiter)) {
                        transferred = true;
                        break;
                    }
                }
            }

            if (!transferred) {
                count += 1;
            }
        }
    }

private:
    isize count = 0;
};

class Event : public SyncPrimitive {
public:
    explicit Event(bool signaled = false, std::source_location location = std::source_location::current())
        : SyncPrimitive(SyncPrimitive::Kind::EVENT), signaled(signaled) {
        this->location = location;
    }

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    struct WaitAwaiter : WaitNode {
        explicit WaitAwaiter(Event &owner) : WaitNode(AsyncNode::NodeKind::EVENT_WAITER), owner(&owner) {}

        bool await_ready() noexcept { return owner->is_set(); }

        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> h, std::source_location location = std::source_location::current()) noexcept {
            owner->insert(this);
            return attach(h.promise(), location);
        }

        Outcome<void, void, Cancellation> await_resume() noexcept {
            if (this->state == AsyncNode::CANCELLED) {
                return outcome_cancel(Cancellation{});
            }

            return {};
        }

    private:
        Event *owner = nullptr;
    };

    /// Waits until the Event is set. If the current wait queue is interrupted,
    /// Cancellation is propagated through the returned Task.
    Task<> wait() {
        auto result = co_await WaitAwaiter(*this);
        if (result.is_cancelled()) {
            co_await cancel();
        }
    }

    void set() noexcept {
        signaled = true;
        drain_waiters([this](WaitNode &waiter) { resume_waiter(waiter); });
    }

    void reset() noexcept { signaled = false; }

    /// Interrupts the current wait queue without changing the signaled state.
    void interrupt() noexcept {
        drain_waiters([this](WaitNode &waiter) { cancel_waiter(waiter); });
    }

    bool is_set() const noexcept { return signaled; }

private:
    bool signaled = false;
};

class ConditionVariable : public SyncPrimitive {
public:
    ConditionVariable(std::source_location location = std::source_location::current())
        : SyncPrimitive(SyncPrimitive::Kind::CONDITION_VARIABLE) {
        this->location = location;
    }

    ConditionVariable(const ConditionVariable &) = delete;
    ConditionVariable &operator=(const ConditionVariable &) = delete;

    struct WaitAwaiter : WaitNode {
        /// Reuses EventWaiter kind - see Semaphore::AcquireAwaiter comment.
        explicit WaitAwaiter(ConditionVariable &owner) : WaitNode(AsyncNode::NodeKind::EVENT_WAITER), owner(&owner) {}

        bool await_ready() const noexcept { return false; }

        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> h, std::source_location location = std::source_location::current()) noexcept {
            owner->insert(this);
            return attach(h.promise(), location);
        }

        void await_resume() noexcept {}

    private:
        ConditionVariable *owner = nullptr;
    };

    /// Atomically unlocks `m`, waits for a notification, then re-locks `m`.
    ///
    /// Cancellation note: if this Task is cancelled while suspended on the
    /// WaitAwaiter, the Mutex will NOT be re-acquired (co_await m.lock() is
    /// never reached). In normal usage this is safe because Cancellation
    /// propagates upward - the caller is also cancelled and never observes
    /// the unlocked Mutex. However, if Cancellation is intercepted externally
    /// (e.g., catch_cancel() or with_token() wrapping a cv.wait() call), the
    /// caller resumes with the Mutex NOT held. Avoid intercepting Cancellation
    /// around cv.wait().
    Task<> wait(Mutex &m) {
        m.unlock();
        co_await WaitAwaiter(*this);
        co_await m.lock();
    }

    void notify_one() {
        while (auto *waiter = pop_waiter()) {
            if (resume_waiter(*waiter)) {
                break;
            }
        }
    }

    void notify_all() {
        drain_waiters([this](WaitNode &waiter) { resume_waiter(waiter); });
    }
};

} // namespace lighter
