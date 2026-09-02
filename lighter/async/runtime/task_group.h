#pragma once

#include <contracts>
#include <source_location>
#include <type_traits>
#include <vector>

#if LIGHTER_ENABLE_EXCEPTIONS
#include <exception>
#endif

#include <lighter/types.hpp>
#include <lighter/utils/config.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/traits.h>
#include <lighter/async/vocab/outcome.h>

namespace lighter {

template <typename... Errors>
struct TaskGroup : AggregateOp {
    using error_type = detail::task_group_error_type_t<Errors...>;
    using result_type = std::conditional_t<std::is_void_v<error_type>, void, Outcome<void, std::vector<error_type>, void>>;

    explicit TaskGroup([[maybe_unused]] EventLoop &loop) : AggregateOp(NodeKind::TASK_GROUP) {}

    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;
    TaskGroup(TaskGroup &&) = delete;
    TaskGroup &operator=(TaskGroup &&) = delete;

    ~TaskGroup() {
        for (auto *child : children) {
            // Null slots are tombstones of eagerly reclaimed children.
            if (!child) {
                continue;
            }
            contract_assert(child->kind == AsyncNode::NodeKind::TASK);
            contract_assert(child->state == AsyncNode::FINISHED || child->state == AsyncNode::CANCELLED ||
                            child->state == AsyncNode::FAILED);
            static_cast<TaskFrame *>(child)->handle().destroy();
        }
    }

    template <typename T, typename E, typename C>
        requires std::is_void_v<E> || is_one_of<E, Errors...>
    bool spawn(Task<T, E, C> &&t) {
        if (decided() || settled) {
            return false;
        }

        // Compact the tombstones left by eager child-frame reclamation once
        // they outnumber the live slots, so a long-lived group stays bounded
        // by its in-flight children rather than its lifetime spawn count.
        if (reclaimed * 2 > children.size()) {
            compact_children();
        }

        auto *node = detail::node_from(t);
        node->intercept_cancel();

        children.reserve(children.size() + 1);
        error_handlers.reserve(error_handlers.size() + 1);

        t.release();
        ++pending;
        children.push_back(node);
        error_handlers.push_back(&extract_error<T, E>);

        auto handle = node->start(*this, std::source_location::current());
        AsyncNode::resume_and_drain(handle);
        return true;
    }

    void cancel() {
        if (decided() || settled) {
            return;
        }
        {
            // The cascade may complete every child synchronously; the Pin
            // keeps those completions from settling mid-loop.
            Pin held(*this);
            decide(Decision::RESUME);
            cancel_children();
        }
        AsyncNode::resume_and_drain(settle_if_idle());
    }

    auto join() { return JoinAwaiter{*this}; }

private:
    using error_handler_fn = void (*)(AsyncNode &child, TaskGroup &group);

    struct JoinAwaiter {
        TaskGroup &group;

        bool await_ready() noexcept {
            contract_assert(!group.joined);
            if (group.pending == 0) {
                group.settled = true;
                group.state = FINISHED;
                return true;
            }
            return false;
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h,
                                              std::source_location location = std::source_location::current()) noexcept {
            contract_assert(!group.settled);
            contract_assert(group.pending > 0);
            group.location = location;
            auto *parent_node = static_cast<AsyncNode *>(&h.promise());
            contract_assert(parent_node->is_task_frame());
            static_cast<TaskFrame *>(parent_node)->set_child(&group);
            group.parent = parent_node;
            group.state = RUNNING;

            // Cancellation checkpoint: the parent was cancelled while it was
            // executing, so its cancel cascade could not reach this group
            // (it was not attached yet). Unlike the other checkpoints, the
            // children are already running and must complete structurally:
            // cancel them now and settle once the last one finishes.
            if (parent_node->state == CANCELLED) {
                group.state = CANCELLED;
                {
                    Pin held(group);
                    group.cancel_children();
                }
                return group.settle_if_idle();
            }

            return std::noop_coroutine();
        }

        result_type await_resume() {
            group.joined = true;
            group.collect_errors();

#if LIGHTER_ENABLE_EXCEPTIONS
            if (!group.exceptions.empty()) {
                std::rethrow_exception(group.exceptions.front());
            }
#endif

            if constexpr (!std::is_void_v<error_type>) {
                if (!group.errors.empty()) {
                    return result_type(outcome_error(std::move(group.errors)));
                }
                return result_type();
            }
        }
    };

    void collect_errors() {
        contract_assert(children.size() == error_handlers.size());
        for (usize i = 0; i < children.size(); ++i) {
            if (children[i] && children[i]->state == AsyncNode::FAILED) {
                error_handlers[i](*children[i], *this);
            }
        }
    }

    void compact_children() noexcept {
        usize live = 0;
        for (usize i = 0; i < children.size(); ++i) {
            if (children[i]) {
                children[live] = children[i];
                error_handlers[live] = error_handlers[i];
                live += 1;
            }
        }
        children.resize(live);
        error_handlers.resize(live);
        reclaimed = 0;
    }

    template <typename T, typename E>
    static void extract_error(AsyncNode &child, TaskGroup &g) {
        if (child.propagated_exception) {
#if LIGHTER_ENABLE_EXCEPTIONS
            g.exceptions.push_back(child.propagated_exception);
#endif
            return;
        }

        if constexpr (!std::is_void_v<E>) {
            auto *promise = static_cast<TaskPromiseObject<T, E> *>(static_cast<TaskFrame *>(&child));
            if (promise->value.has_value() && promise->value->has_error()) {
                if constexpr (std::is_same_v<E, error_type>) {
                    g.errors.push_back(std::move(*promise->value).error());
                } else {
                    g.errors.push_back(error_type(std::move(*promise->value).error()));
                }
            }
        }
    }

    bool joined = false;

    std::vector<error_handler_fn> error_handlers;

    std::conditional_t<std::is_void_v<error_type>, std::type_identity<void>, std::vector<error_type>> errors;

#if LIGHTER_ENABLE_EXCEPTIONS
    std::vector<std::exception_ptr> exceptions;
#endif
};

TaskGroup(EventLoop &) -> TaskGroup<>;

} // namespace lighter
