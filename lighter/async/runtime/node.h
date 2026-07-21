#pragma once

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <source_location>
#include <vector>

#include <lighter/scalar_types.hpp>
#include <lighter/utils/config.h>

namespace lighter {

class SyncPrimitive;
class TaskFrame;

/// Type-erased base for all coroutine-related nodes in the Task tree.
///
/// This hierarchy models awaitable runtime entities only.
/// Shared sync resources (Mutex/Event/Semaphore/cv) live outside it and are
/// referenced by WaitNode nodes while a Task is blocked on them.
class AsyncNode {
public:
    enum class NodeKind : u8 {
        TASK,

        /// Wait queue entries - WaitNode subclasses.
        /// Semaphore and CV reuse EventWaiter (identical cancel semantics).
        MUTEX_WAITER,
        EVENT_WAITER,

        /// Aggregate operations - WhenAll / WhenAny / TaskGroup.
        WHEN_ALL,
        WHEN_ANY,
        TASK_GROUP,

        /// PENDING libuv I/O - timers, signals, fs, network, etc.
        SYSTEM_IO,
    };

    enum Policy : u8 {
        NONE = 0,
        /// Reserved for future use.
        EXPLICIT_CANCEL = 1 << 0,
        /// When set, Cancellation of this node does NOT fail upward.
        /// The parent resumes normally and can inspect the cancelled state.
        /// Used by catch_cancel() and with_token().
        INTERCEPT_CANCEL = 1 << 1,
    };

    enum State : u8 {
        PENDING,
        RUNNING,
        CANCELLED,
        FINISHED,
        FAILED,
    };

    const NodeKind kind;

    Policy policy = NONE;

    State state = PENDING;

    std::source_location location;

    bool is_task_frame() const noexcept {
        return kind == NodeKind::TASK;
    }

    bool is_wait_node() const noexcept {
        return NodeKind::MUTEX_WAITER <= kind && kind <= NodeKind::EVENT_WAITER;
    }

    bool is_aggregate_op() const noexcept {
        return NodeKind::WHEN_ALL <= kind && kind <= NodeKind::TASK_GROUP;
    }

    bool is_finished() const noexcept {
        return state == FINISHED;
    }

    bool is_cancelled() const noexcept {
        return state == CANCELLED;
    }

    bool is_failed() const noexcept {
        return state == FAILED;
    }

    // Keep this out-of-line. clang -O3 miscompiles direct promise policy writes in
    // coroutine return-object conversions, which can drop INTERCEPT_CANCEL. See also
    // https://github.com/llvm/llvm-project/issues/105595. Fixed in clang 21.
    void intercept_cancel() noexcept;

    void cancel();

    void resume();

    std::coroutine_handle<> attach(AsyncNode &parent, std::source_location location);

    std::coroutine_handle<> finalize();

private:
    /// Cancellation-checkpoint path of attach(): the awaiting Task was
    /// cancelled while it was executing, so instead of starting new work
    /// under it, finalize it at this suspension point.
    std::coroutine_handle<> attach_cancelled(TaskFrame &parent);

public:
    std::coroutine_handle<> on_child_complete(AsyncNode &child);

    static void resume_and_drain(std::coroutine_handle<> handle);

protected:
    explicit AsyncNode(NodeKind k) : kind(k) {}

public:
    std::exception_ptr propagated_exception;
};

class TaskFrame : public AsyncNode {
protected:
    friend class AsyncNode;

    explicit TaskFrame() : AsyncNode(NodeKind::TASK) {}

public:
    bool root = false;

    /// Optional hook invoked when a child Task fails, allowing the parent to
    /// intercept the Error before normal resumption. Used by OrFailTaskAwait
    /// to propagate errors directly without resuming the parent coroutine.
    using error_hook = std::coroutine_handle<> (*)(AsyncNode &child, AsyncNode &parent);

    std::coroutine_handle<> handle() {
        return std::coroutine_handle<>::from_address(address);
    }

    bool has_child() const noexcept {
        return child != nullptr;
    }

    void set_child(AsyncNode *node) noexcept {
        child = node;
    }

    /// A Task's child pointer distinguishes three situations:
    ///   - child == some node: suspended, awaiting that node;
    ///   - child == this (sentinel): the coroutine is executing on the stack
    ///     (or scheduled to); cancel() must not finalize it - the frame
    ///     observes `state == CANCELLED` at its next suspension point;
    ///   - child == nullptr: Idle - not executing and awaiting nothing.
    bool is_executing() const noexcept {
        return child == this;
    }

    void mark_executing() noexcept {
        child = this;
    }

    void set_error_hook(error_hook fn) noexcept {
        error_hook_fn = fn;
    }

    error_hook get_error_hook() const noexcept {
        return error_hook_fn;
    }

    void clear_error_hook() noexcept {
        error_hook_fn = nullptr;
    }

    const AsyncNode *get_parent() const noexcept {
        return parent;
    }

    const AsyncNode *get_child() const noexcept {
        return child;
    }

protected:
    /// Stores the raw address of the coroutine frame (handle).
    ///
    /// Theoretically, this is redundant because the promise object is embedded
    /// within the coroutine frame. However, deriving the frame address from `this`
    /// (via `from_promise`) requires knowing the concrete Promise type to account
    /// for the opaque compiler overhead (e.g., resume/destroy Function pointers)
    /// located before the promise.
    ///
    /// Since this base class is type-erased, we cannot calculate that offset dynamically
    /// and must explicitly cache the handle address here (costing 1 pointer size).
    void *address = nullptr;

private:
    AsyncNode *parent = nullptr;

    AsyncNode *child = nullptr;

    error_hook error_hook_fn = nullptr;
};

class WaitNode : public AsyncNode {
public:
    friend class AsyncNode;
    friend class SyncPrimitive;

    explicit WaitNode(NodeKind k) : AsyncNode(k) {}

    const AsyncNode *get_parent() const noexcept {
        return parent;
    }

    const SyncPrimitive *get_resource() const noexcept {
        return resource;
    }

    const WaitNode *get_next() const noexcept {
        return next;
    }

protected:
    using abandon_fn = void (*)(void *) noexcept;

    /// The SyncPrimitive this waiter is queued on (nullptr if not queued).
    SyncPrimitive *resource = nullptr;

    /// Intrusive doubly-linked list pointers for the SyncPrimitive's wait queue.
    WaitNode *prev = nullptr;
    WaitNode *next = nullptr;

    AsyncNode *parent = nullptr;

    void *abandon_context = nullptr;

    abandon_fn abandon = nullptr;
};

/// Base for WhenAll / WhenAny / TaskGroup.
///
/// The aggregate settles (delivers its Outcome to the parent) exactly when
/// `pending` drops to zero. `pending` counts unfinished children plus one Pin
/// for every stack frame that is still iterating over `children` (the arming
/// loop in await_suspend and every cancel cascade). Re-entrant child
/// completions during those loops therefore only decrement the counter; they
/// can never resume the parent from inside a loop that still touches this
/// object.
///
/// The Outcome itself is not latched as a separate "what to deliver" value;
/// it is derived in settle() from `decision`, `state` and the attribution
/// indices, so the delivered Outcome and its attribution cannot diverge.
class AggregateOp : public AsyncNode {
protected:
    friend class AsyncNode;

    explicit AggregateOp(NodeKind k) : AsyncNode(k) {}

public:
    const AsyncNode *get_parent() const noexcept {
        return parent;
    }

    const std::vector<AsyncNode *> &get_children() const noexcept {
        return children;
    }

protected:
    /// The first Event that picked this aggregate's Outcome. Latched once,
    /// with one exception: a child Error upgrades any earlier decision,
    /// because an Error must never be dropped silently.
    ///
    /// An external cancel() is tracked separately as `state == CANCELLED`
    /// (set by cancel() before it dispatches on the node kind). settle()
    /// treats it like a Cancel decision: it upgrades a plain RESUME, but a
    /// child Error still outranks it.
    enum class Decision : u8 {
        /// Undecided. A WhenAll whose children all succeed settles as
        /// success without ever recording a decision.
        NONE,

        /// Resume the parent normally (WhenAny winner; TaskGroup child
        /// failure, which is reported via join() instead).
        RESUME,

        /// A child's un-intercepted Cancellation cancels the aggregate.
        CANCEL,

        /// A child failed with a structured Error or exception.
        FAILURE,
    };

    /// Sentinel value for WhenAny: no winner yet.
    constexpr static usize k_npos = (std::numeric_limits<usize>::max)();

    AsyncNode *parent = nullptr;

    std::vector<AsyncNode *> children;

    /// Unfinished children plus active stack pins. Zero means "safe to settle".
    usize pending = 0;

    /// Index of the first child to finish (WhenAny only).
    usize winner = k_npos;

    /// Index of the first child to finish with a structured Error or exception.
    usize first_error_child = k_npos;

    /// Index of the first child to finish with Cancellation.
    usize first_cancel_child = k_npos;

    Decision decision = Decision::NONE;

    /// Set once the Outcome has been delivered to the parent.
    bool settled = false;

    /// Number of tombstoned (null) slots in `children` left behind by eager
    /// child-frame reclamation. Used by TaskGroup only: completed children
    /// that join() will never inspect again are destroyed on completion
    /// instead of accumulating until the group is destroyed.
    usize reclaimed = 0;

    /// Keeps `pending` above zero while a loop over `children` is on the
    /// stack. The holder must call settle_if_idle() after the Pin dies.
    struct Pin {
        AggregateOp &op;

        explicit Pin(AggregateOp &op) noexcept : op(op) {
            op.pending += 1;
        }

        ~Pin() {
            op.pending -= 1;
        }
    };

    /// True once an Outcome has been picked (or an external cancel arrived);
    /// implies the children cancel cascade has already been triggered.
    bool decided() const noexcept {
        return decision != Decision::NONE || state == CANCELLED;
    }

    /// Latches `d` as the Outcome and cancels the remaining children.
    ///
    /// The caller must guarantee `pending > 0` for the duration of the
    /// cascade - either by holding a Pin, or by being the completion handler
    /// of a child that has not been counted off yet.
    void decide(Decision d) {
        decision = d;
        cancel_children();
    }

    /// Cancels every child that has not reached a terminal state yet.
    /// Idempotent: terminal children ignore cancel(). Null slots are
    /// tombstones of reclaimed TaskGroup children.
    void cancel_children() {
        for (auto *child : children) {
            if (child) {
                child->cancel();
            }
        }
    }

    usize find_child_index(const AsyncNode &child) const {
        auto it = std::ranges::find(children, &child);
        assert(it != children.end() && "child not found in aggregate");
        if (it == children.end())
            std::abort();
        return static_cast<usize>(it - children.begin());
    }

    /// Rethrows the propagated exception if one was captured from a failed child.
    void rethrow_if_propagated() {
#if LIGHTER_ENABLE_EXCEPTIONS
        if (propagated_exception) {
            std::rethrow_exception(propagated_exception);
        }
#endif
    }

    /// Settles if all children completed, no pins are held, and a parent is
    /// attached (a TaskGroup settles only after join()). Returns the
    /// coroutine to resume, or noop.
    std::coroutine_handle<> settle_if_idle() noexcept;

    /// Delivers the final Outcome to the parent. Runs exactly once.
    std::coroutine_handle<> settle() noexcept;

    std::coroutine_handle<> arm_and_resume(AsyncNode &parent_node,
                                           std::source_location loc) noexcept {
        this->location = loc;

        assert(parent_node.is_task_frame() && "aggregate parent must be a Task");

        // Cancellation checkpoint: don't start any children under a parent
        // that is already cancelled. The unstarted child tasks are destroyed
        // together with the parent frame that owns this aggregate.
        if (parent_node.state == CANCELLED) {
            auto *p = static_cast<TaskFrame *>(&parent_node);
            p->set_child(nullptr);
            return p->finalize();
        }

        static_cast<TaskFrame *>(&parent_node)->set_child(this);

        parent = &parent_node;
        pending = children.size();
        winner = k_npos;
        first_error_child = k_npos;
        first_cancel_child = k_npos;
        decision = Decision::NONE;
        settled = false;
        propagated_exception = nullptr;
        state = RUNNING;

        {
            Pin held(*this);

            for (auto *child : children) {
                assert(child && "aggregate contains a null child");
                child->attach(*this, location);
            }

            for (auto *child : children) {
                if (decided()) {
                    // A synchronous completion already picked the Outcome and
                    // cancelled the remaining children; resuming a cancelled
                    // child would deliver a second completion.
                    break;
                }
                child->resume();
            }
        }

        // If every child completed synchronously, resume the parent now via
        // symmetric transfer.
        return settle_if_idle();
    }
};

class IoOp : public AsyncNode {
protected:
    friend class AsyncNode;

    using on_cancel = void (*)(IoOp *self);

    explicit IoOp(NodeKind k = NodeKind::SYSTEM_IO) : AsyncNode(k) {}

    /// Callback invoked when this operation is cancelled (e.g. to close a uv handle).
    on_cancel action = nullptr;

    AsyncNode *parent = nullptr;

public:
    void complete() noexcept;

    const AsyncNode *get_parent() const noexcept {
        return parent;
    }
};

} // namespace lighter
