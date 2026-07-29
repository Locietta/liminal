#include "node.h"

#include <contracts>
#include <utility>
#include <vector>

#include <lighter/async/detail/libuv_helper.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/sync.h>

namespace lighter {

namespace {

#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
thread_local std::vector<std::coroutine_handle<>> pending_frame_destroys;
#endif

#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
void drain_pending_destroys() {
    while (!pending_frame_destroys.empty()) {
        auto queued = std::move(pending_frame_destroys);
        pending_frame_destroys.clear();
        for (auto handle : queued) {
            contract_assert(handle != nullptr);
            handle.destroy();
        }
    }
}
#endif

} // namespace

void AsyncNode::resume_and_drain(std::coroutine_handle<> handle) {
    static thread_local bool draining = false;

    const bool outermost = !draining;
    if (outermost) {
        draining = true;
    }

    handle.resume();
#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
    drain_pending_destroys();
#endif

    if (outermost && EventLoop::has_current()) {
        EventLoop::current().drain_deferred();
    }

    if (outermost) {
        draining = false;
    }
}

void AsyncNode::intercept_cancel() noexcept { policy = static_cast<Policy>(policy | INTERCEPT_CANCEL); }

std::coroutine_handle<> AggregateOp::settle_if_idle() noexcept {
    if (pending != 0 || settled || !parent) {
        return std::noop_coroutine();
    }
    return settle();
}

/// Outcome precedence, derived from the recorded facts (errors are never
/// dropped, even when an external cancel() raced the failing child - the
/// same rule trio and Kotlin coroutines use for real errors vs Cancellation):
///   1. a child Error;
///   2. any Cancellation - resumed with the cancel Outcome when
///      intercepting, propagated upward via finalize otherwise;
///   3. success.
std::coroutine_handle<> AggregateOp::settle() noexcept {
    settled = true;

    auto *p = static_cast<TaskFrame *>(parent);
    parent = nullptr;

    const bool intercepts = policy & INTERCEPT_CANCEL;
    const bool external_cancel = state == CANCELLED;

    if (decision == Decision::FAILURE) {
        state = FAILED;
        p->mark_executing();
        return p->handle();
    }

    if (external_cancel || decision == Decision::CANCEL) {
        state = CANCELLED;
        if (intercepts) {
            p->mark_executing();
            return p->handle();
        }
        p->set_child(nullptr);
        p->state = CANCELLED;
        return p->finalize();
    }

    state = FINISHED;
    p->mark_executing();
    return p->handle();
}

/// Recursively cancels this node and all of its descendants.
/// Idempotent: re-cancelling an already-terminal node is a no-op.
void AsyncNode::cancel() {
    if (state == CANCELLED || state == FAILED || state == FINISHED) {
        return;
    }
    state = CANCELLED;

    switch (kind) {
        case NodeKind::TASK: {
            auto *self = static_cast<TaskFrame *>(this);
            if (self->is_executing()) {
                // The frame is live on the stack (or scheduled); it observes
                // state == CANCELLED at its next suspension point.
                break;
            }
            if (self->child) {
                self->child->cancel();
            } else if (self->parent) {
                auto next = self->finalize();
                AsyncNode::resume_and_drain(next);
            }
            break;
        }
        case NodeKind::MUTEX_WAITER:
        case NodeKind::EVENT_WAITER: {
            auto *self = static_cast<WaitNode *>(this);
            if (auto *res = self->resource) {
                res->remove(self);
            }
            auto *p = self->parent;
            self->parent = nullptr;
            contract_assert(p != nullptr);
            auto next = p->on_child_complete(*self);
            AsyncNode::resume_and_drain(next);
            break;
        }

        case NodeKind::WHEN_ALL:
        case NodeKind::WHEN_ANY:
        case NodeKind::TASK_GROUP: {
            auto *self = static_cast<AggregateOp *>(this);

            // state == CANCELLED (set above) is the record of this external
            // cancel; settle() derives the Outcome from it. The cascade can
            // complete every remaining child synchronously, so hold a Pin
            // while it runs and settle only afterwards.
            {
                AggregateOp::Pin held(*self);
                self->cancel_children();
            }

            auto next = self->settle_if_idle();
            AsyncNode::resume_and_drain(next);
            break;
        }

        case NodeKind::SYSTEM_IO: {
            auto *self = static_cast<IoOp *>(this);
            contract_assert(self->action != nullptr);
            self->action(self);
            break;
        }
    }
}

/// Resumes a Task's coroutine, or finalizes it if already cancelled/failed.
///
/// The cancel/fail path handles the case where a sync primitive deferred
/// this resume and Cancellation arrived before the deferred tick fired.
/// If the child WaitNode was resolved by resume_waiter (state == FINISHED),
/// its grant (e.g. a Mutex lock) is abandoned before finalization.
void AsyncNode::resume() {
    if (is_task_frame()) {
        auto *f = static_cast<TaskFrame *>(this);
        if (is_cancelled() || is_failed()) {
            auto *ch = f->child;
            if (ch && ch->is_wait_node() && ch->is_finished()) {
                auto *wn = static_cast<WaitNode *>(ch);
                if (wn->abandon) {
                    auto fn = wn->abandon;
                    auto ctx = wn->abandon_context;
                    wn->abandon = nullptr;
                    wn->abandon_context = nullptr;
                    fn(ctx);
                }
            }
            f->set_child(nullptr);
            if (f->get_parent()) {
                auto next = f->finalize();
                resume_and_drain(next);
            }
            return;
        }
        f->mark_executing();
        f->handle().resume();
#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
        drain_pending_destroys();
#endif
    }
}

/// Called by libuv callbacks when an I/O operation completes.
/// Preserves CANCELLED state if already set, then notifies the parent.
void IoOp::complete() noexcept {
    if (state != CANCELLED) {
        state = FINISHED;
    }
    auto *p = parent;
    parent = nullptr;
    auto next = p->on_child_complete(*this);
    AsyncNode::resume_and_drain(next);
}

/// Cancellation checkpoint: the parent observes its Cancellation at the next
/// suspending co_await instead of starting new work (trio-style prompt
/// Cancellation). Everything below only returns coroutine handles - nothing
/// is resumed while the parent's await_suspend is still on the stack; the
/// compiler performs the transfer after it returns.
std::coroutine_handle<> AsyncNode::attach_cancelled(TaskFrame &parent) {
    switch (kind) {
        case NodeKind::TASK:
            // Never started; the frame is destroyed together with the parent
            // frame that owns the Task object.
            parent.set_child(nullptr);
            return parent.finalize();

        case NodeKind::MUTEX_WAITER:
        case NodeKind::EVENT_WAITER: {
            auto *self = static_cast<WaitNode *>(this);
            if (auto *res = self->resource) {
                res->remove(self);
            }
            parent.set_child(nullptr);
            return parent.finalize();
        }

        case NodeKind::WHEN_ALL:
        case NodeKind::WHEN_ANY:
        case NodeKind::TASK_GROUP:
            // Aggregates run their checkpoints in arm_and_resume / join.
            break;

        case NodeKind::SYSTEM_IO: {
            // The uv request may already be in flight, so it cannot be
            // abandoned synchronously: link the parent, then cancel the
            // operation. Its completion - synchronous or via a later uv
            // callback - propagates through on_child_complete and finalizes
            // the parent with structured completion intact. cancel() may
            // destroy this node and the parent frame; touch nothing after.
            auto *self = static_cast<IoOp *>(this);
            self->parent = &parent;
            parent.set_child(this);
            this->cancel();
            return std::noop_coroutine();
        }
    }

    std::abort();
}

/// Wires this node as a child of `parent_node`. For Task nodes, sets state
/// to RUNNING and returns the coroutine handle (ready to resume).
/// For transient nodes (WaitNode, IoOp), records the parent
/// and returns noop_coroutine (resumed later by Event/complete).
std::coroutine_handle<> AsyncNode::attach(AsyncNode &parent_node, std::source_location loc) {
    this->location = loc;

    if (parent_node.is_task_frame() && parent_node.state == CANCELLED) {
        return attach_cancelled(static_cast<TaskFrame &>(parent_node));
    }

    if (parent_node.kind == NodeKind::TASK) {
        static_cast<TaskFrame *>(&parent_node)->child = this;
    }

    switch (this->kind) {
        case NodeKind::TASK: {
            auto self = static_cast<TaskFrame *>(this);
            self->state = RUNNING;
            self->parent = &parent_node;
            return self->handle();
        }

        case NodeKind::MUTEX_WAITER:
        case NodeKind::EVENT_WAITER: {
            auto self = static_cast<WaitNode *>(this);
            self->parent = &parent_node;
            return std::noop_coroutine();
        }
        case NodeKind::WHEN_ALL:
        case NodeKind::WHEN_ANY:
        case NodeKind::TASK_GROUP: break;
        case NodeKind::SYSTEM_IO: {
            auto self = static_cast<IoOp *>(this);
            self->parent = &parent_node;
            return std::noop_coroutine();
        }
    }

    std::abort();
}

/// Called when a Task reaches final_suspend (FINISHED, CANCELLED, or FAILED).
/// For root tasks with no parent, destroys the coroutine frame.
/// Otherwise, notifies the parent via on_child_complete.
std::coroutine_handle<> AsyncNode::finalize() {
    switch (kind) {
        case NodeKind::TASK: {
            auto self = static_cast<TaskFrame *>(this);
            if (!self->parent) {
                if (self->root) {
#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
                    pending_frame_destroys.push_back(self->handle());
#else
                    self->handle().destroy();
#endif
                }
                return std::noop_coroutine();
            }

            // Sever the parent link before delivering the completion: a
            // completion is delivered at most once, and resume()/cancel()
            // recognize an already-finalized Task by its null parent.
            auto *p = self->parent;
            self->parent = nullptr;
            return p->on_child_complete(*self);
        }

        case NodeKind::MUTEX_WAITER:
        case NodeKind::EVENT_WAITER:
        case NodeKind::WHEN_ALL:
        case NodeKind::WHEN_ANY:
        case NodeKind::TASK_GROUP:
        case NodeKind::SYSTEM_IO: break;
    }

    std::abort();
}

/// Dispatches a child's completion to its parent node.
///
/// For Task parents: resumes the coroutine normally for FINISHED/FAILED,
///   or propagates Cancellation upward.
/// For Aggregate parents (WhenAll/WhenAny/TaskGroup): records the Event
///   (Error / Cancellation / winner), cancels the remaining children on the
///   first deciding Event, counts the child off, and settles once every
///   child has completed (structured completion).
std::coroutine_handle<> AsyncNode::on_child_complete(AsyncNode &child) {
    switch (kind) {
        case NodeKind::TASK: {
            auto self = static_cast<TaskFrame *>(this);

            if (child.state == CANCELLED) {
                if (child.policy & INTERCEPT_CANCEL) {
                    self->mark_executing();
                    return self->handle();
                }

                self->set_child(nullptr);
                self->state = CANCELLED;
                return self->finalize();
            }

            self->mark_executing();
            if (child.state == FAILED && child.kind == NodeKind::TASK) {
                auto *child_task = static_cast<TaskFrame *>(&child);
                if (auto propagate = child_task->get_error_hook()) {
                    return propagate(child, *self);
                }
            }
            return self->handle();
        }

        case NodeKind::WHEN_ALL:
        case NodeKind::WHEN_ANY: {
            auto self = static_cast<AggregateOp *>(this);
            contract_assert(!self->settled);
            contract_assert(self->pending > 0);

            const bool intercepts = self->policy & INTERCEPT_CANCEL;
            // A cancelled child is a Cancellation Event unless the child
            // intercepts its own Cancellation while the aggregate does not.
            const bool cancelled = child.state == CANCELLED && (intercepts || !(child.policy & INTERCEPT_CANCEL));

            // The completing child is not counted off until below, so the
            // cancel cascades inside decide() cannot settle re-entrantly.
            if (child.state == FAILED) {
                if (self->first_error_child == AggregateOp::k_npos) {
                    self->first_error_child = self->find_child_index(child);
                    if (child.propagated_exception) {
                        self->propagated_exception = child.propagated_exception;
                    }
                    // An Error upgrades any earlier decision.
                    self->decide(AggregateOp::Decision::FAILURE);
                }
            } else if (cancelled) {
                // Attribution is recorded independently of the decision: even
                // when a winner already decided, a later external cancel can
                // settle the aggregate as CANCELLED, and await_resume then
                // needs first_cancel_child to extract the cancel value.
                if (intercepts && self->first_cancel_child == AggregateOp::k_npos) {
                    self->first_cancel_child = self->find_child_index(child);
                }
                if (!self->decided()) {
                    self->decide(AggregateOp::Decision::CANCEL);
                }
            } else if (self->kind == NodeKind::WHEN_ANY && !self->decided()) {
                self->winner = self->find_child_index(child);
                self->decide(AggregateOp::Decision::RESUME);
            }

            self->pending -= 1;
            return self->settle_if_idle();
        }

        case NodeKind::TASK_GROUP: {
            auto *self = static_cast<AggregateOp *>(this);
            contract_assert(!self->settled);
            contract_assert(self->pending > 0);

            if (child.state == FAILED) {
                // A failed child aborts the group, and its Error must survive
                // even a racing external cancel (errors outrank Cancellation):
                // Decision::FAILURE makes settle() resume the joiner, which
                // collects the Error, instead of propagating the cancel past
                // it. The completing child still pins `pending`, so the
                // cascade cannot settle re-entrantly.
                const bool first_abort = !self->decided();
                self->decision = AggregateOp::Decision::FAILURE;
                if (first_abort) {
                    self->cancel_children();
                }
            } else if (child.state == CANCELLED && !self->decided()) {
                // A cancelled child aborts the group: cancel the remaining
                // children, but settle as a normal resume - join() reports
                // the collected errors.
                self->decide(AggregateOp::Decision::RESUME);
            }

            // Eagerly reclaim the completed child's frame unless join()
            // still needs it for Error extraction. The slot is tombstoned
            // (walkers and the cancel cascade skip null children) and
            // compacted by the next spawn(). Destroying the frame from its
            // own final suspension is the same pattern finalize() uses for
            // root tasks.
            if (child.state != FAILED) {
                contract_assert(child.kind == NodeKind::TASK);
                self->children[self->find_child_index(child)] = nullptr;
                self->reclaimed += 1;
                auto handle = static_cast<TaskFrame &>(child).handle();
#if LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF
                pending_frame_destroys.push_back(handle);
#else
                handle.destroy();
#endif
            }

            self->pending -= 1;
            return self->settle_if_idle();
        }

        case NodeKind::MUTEX_WAITER:
        case NodeKind::EVENT_WAITER:
        case NodeKind::SYSTEM_IO:
        default: {
            std::abort();
        }
    }
}

} // namespace lighter
