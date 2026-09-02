#pragma once

#include <concepts>
#include <contracts>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <optional>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

#include <lighter/utils/config.h>
#include <lighter/utils/panic.h>
#include <lighter/utils/type_traits.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/vocab/awaitable.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/outcome.h>

namespace lighter {

// ============================================================================
// PromiseResult - two specializations
// ============================================================================

/// General case: stores Outcome<T, E, void>.
/// C is never stored in the promise - Cancellation uses Task state.
/// Layout depends only on T and E; Cancellation uses Task state.
template <typename T, typename E, typename C>
struct PromiseResult {
    std::optional<Outcome<T, E, void>> value;

    bool has_error_result() const noexcept {
        if constexpr (std::is_void_v<E>) {
            return false;
        } else {
            return value.has_value() && value->has_error();
        }
    }

    template <typename U>
    void return_value(U &&val) noexcept {
        value.emplace(std::forward<U>(val));
    }
};

/// Void-value tasks complete successfully via `co_return;`.
/// Use `co_await fail(...)` or `co_await or_fail(...)` to propagate errors.
template <typename E, typename C>
struct PromiseResult<void, E, C> {
    std::optional<Outcome<void, E, void>> value;

    bool has_error_result() const noexcept {
        if constexpr (std::is_void_v<E>) {
            return false;
        } else {
            return value.has_value() && value->has_error();
        }
    }

    void return_void() noexcept { value.emplace(); }
};

// ============================================================================
// PromiseException, TransitionAwait, cancel()
// ============================================================================

struct PromiseException {
#if LIGHTER_ENABLE_EXCEPTIONS
    bool has_exception() const noexcept { return exception != nullptr; }

    std::exception_ptr get_exception() const noexcept { return exception; }

    void unhandled_exception() noexcept { this->exception = std::current_exception(); }

    void rethrow_if_exception() {
        if (this->exception) {
            std::rethrow_exception(this->exception);
        }
    }

protected:
    std::exception_ptr exception{nullptr};
#else
    bool has_exception() const noexcept { return false; }

    std::exception_ptr get_exception() const noexcept { return nullptr; }

    void unhandled_exception() { std::abort(); }

    void rethrow_if_exception() {}
#endif
};

struct TransitionAwait {
    AsyncNode::State state = AsyncNode::PENDING;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        auto &promise = handle.promise();
        if (state == AsyncNode::FINISHED) {
            if (promise.state == AsyncNode::FAILED) {
                return promise.finalize();
            }
            contract_assert(promise.state == AsyncNode::RUNNING || promise.state == AsyncNode::CANCELLED);
            // Real errors outrank Cancellation (trio semantics): a Task that
            // fails or throws after being cancelled still reports the Error.
            // Only a normal completion of a cancelled Task finalizes as
            // CANCELLED.
            if (promise.has_exception()) {
                promise.state = AsyncNode::FAILED;
                promise.propagated_exception = promise.get_exception();
            } else if (promise.has_error_result()) {
                promise.state = AsyncNode::FAILED;
            } else if (promise.state != AsyncNode::CANCELLED) {
                promise.state = state;
            }
        } else if (state == AsyncNode::CANCELLED) {
            promise.state = state;
        } else {
            lighter::panic("unexpected Task state");
        }
        return promise.finalize();
    }

    [[noreturn]] void await_resume() const noexcept { std::abort(); }
};

inline auto cancel() { return TransitionAwait(AsyncNode::CANCELLED); }

/// Carrier for or_fail(); transformed by await_transform in the promise.
template <typename Outcome>
struct OrFailAwait {
    Outcome result;
};

/// An Outcome-like type that carries an Error channel (no cancel channel).
template <typename Outcome>
concept or_fail_result = is_outcome_v<std::remove_cvref_t<Outcome>> && std::is_void_v<typename std::remove_cvref_t<Outcome>::cancel_type> &&
                         (!std::is_void_v<typename std::remove_cvref_t<Outcome>::error_type>);

/// Propagate the Error channel of a non-Task Outcome; resume with its value on success.
///
///   auto value = co_await or_fail(some_result);  // propagates Error or unwraps value
///
template <typename Outcome>
    requires or_fail_result<Outcome>
auto or_fail(Outcome &&result) {
    return OrFailAwait<std::remove_cvref_t<Outcome>>{std::forward<Outcome>(result)};
}

/// Carrier for fail(); holds forwarding references to the Error constructor args.
/// Must be consumed immediately via co_await (same lifetime constraint as
/// std::forward_as_tuple).
template <typename... Args>
struct FailAwait {
    std::tuple<Args &&...> args;
};

/// Construct an Error and transition the current coroutine to FINISHED.
///
///   co_await fail(error_code, "message");  // replaces co_return outcome_error(...)
///
template <typename... Args>
auto fail(Args &&...args) {
    return FailAwait<Args...>{std::forward_as_tuple(std::forward<Args>(args)...)};
}

/// Awaitable returned by await_transform for or_fail(non-Task Outcome).
/// When finish=true (Error path): suspends and transitions to FINISHED.
/// When finish=false (success path): ready immediately, await_resume unwraps the value.
template <typename Outcome>
struct OrFailResumeAwait {
    Outcome result;
    bool finish = false;

    bool await_ready() const noexcept { return !finish; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        return TransitionAwait(AsyncNode::FINISHED).await_suspend(handle);
    }

    decltype(auto) await_resume() noexcept {
        if constexpr (!std::is_void_v<typename Outcome::value_type>) {
            return std::move(*result);
        }
    }
};

// ============================================================================
// Task<T, E, C>
// ============================================================================

template <typename T = void, typename E = void, typename C = void>
struct Task;

namespace detail {

/// Proxy returned by task::or_fail(). Holds the Task until await_transform
/// converts it into an OrFailTaskAwait.
template <typename Task>
struct OrFailProxy {
    Task inner;
};

/// Error hook invoked by on_child_complete when a child Task fails
/// while an OrFailTaskAwait is active. Bypasses normal parent resumption
/// by writing the child's Error directly into the parent promise and
/// transitioning the parent to FINISHED/FAILED.
///
/// If the child threw an exception instead, clears the hook and lets the
/// parent resume normally so await_resume can rethrow via rethrow_if_exception.
template <typename ParentPromise, typename ParentError, typename ChildTask>
std::coroutine_handle<> propagate_fail(AsyncNode &child_node, AsyncNode &parent_node) {
    auto *child = static_cast<typename ChildTask::promise_type *>(&child_node);
    auto *parent = static_cast<ParentPromise *>(&parent_node);
    auto *child_task = static_cast<TaskFrame *>(&child_node);
    auto *parent_task = static_cast<TaskFrame *>(&parent_node);

    child_task->clear_error_hook();

    // Exception: let parent resume normally; await_resume will rethrow.
    if (child->propagated_exception) {
        return parent_task->handle();
    }

    // Error: move into parent and short-circuit to FINISHED.
    contract_assert(child->value.has_value() && child->value->has_error());
    parent->value.emplace(outcome_error(ParentError(std::move(*child->value).error())));
    parent_task->state = AsyncNode::FAILED;
    return parent_task->finalize();
}

/// Awaitable for `co_await Task.or_fail()`. Installs an Error hook on the
/// child Task before suspension. On success, await_resume unwraps the value
/// directly (skipping the Outcome wrapper). On failure, the Error hook
/// fires and the parent never resumes at this point.
template <typename ParentPromise, typename ParentError, typename ChildTask>
struct OrFailTaskAwait {
    typename ChildTask::Awaiter inner;

    bool await_ready() noexcept { return inner.await_ready(); }

    auto await_suspend(std::coroutine_handle<ParentPromise> h, std::source_location location = std::source_location::current()) noexcept {
        inner.awaitee.h.promise().set_error_hook(&propagate_fail<ParentPromise, ParentError, ChildTask>);
        return inner.await_suspend(h, location);
    }

    /// Only reached on success (Error path is intercepted by the hook).
    auto await_resume() {
        inner.awaitee.h.promise().clear_error_hook();
        if constexpr (!std::is_void_v<typename ChildTask::value_type>) {
            auto result = inner.await_resume();
            return typename ChildTask::value_type(std::move(*result));
        } else {
            std::ignore = inner.await_resume();
        }
    }
};

} // namespace detail

template <typename T, typename E>
struct TaskReturnObject;

template <typename T, typename E>
struct TaskPromiseObject : TaskFrame, PromiseResult<T, E, void>, PromiseException {
    using coroutine_handle = std::coroutine_handle<TaskPromiseObject>;

    using PromiseResult<T, E, void>::value;

    auto handle() { return coroutine_handle::from_promise(*this); }

    auto initial_suspend() const noexcept { return std::suspend_always(); }

    auto final_suspend() const noexcept { return TransitionAwait(AsyncNode::FINISHED); }

    auto get_return_object() { return TaskReturnObject<T, E>{handle()}; }

    /// co_await fail(args...): write Error and transition to FINISHED.
    template <typename... Args>
    auto await_transform(FailAwait<Args...> &&fail) noexcept
        requires(!std::is_void_v<E>) && std::constructible_from<E, Args...>
    {
        value.emplace(outcome_error(
            std::apply([](auto &&...forwarded) { return E(std::forward<decltype(forwarded)>(forwarded)...); }, std::move(fail.args))));
        return TransitionAwait(AsyncNode::FINISHED);
    }

    /// co_await or_fail(Outcome): propagate Error or unwrap value (non-task).
    template <typename Outcome>
    auto await_transform(OrFailAwait<Outcome> &&failed) noexcept
        requires(!std::is_void_v<E>) && or_fail_result<Outcome> && std::constructible_from<E, typename Outcome::error_type>
    {
        if (failed.result.has_error()) {
            value.emplace(outcome_error(E(std::move(failed.result).error())));
            return OrFailResumeAwait<Outcome>{std::move(failed.result), true};
        }
        return OrFailResumeAwait<Outcome>{std::move(failed.result), false};
    }

    /// co_await Task.or_fail(): install Error hook for cross-Task propagation.
    template <typename ChildT, typename ChildE>
    auto await_transform(detail::OrFailProxy<Task<ChildT, ChildE, void>> &&wrapped) noexcept
        requires(!std::is_void_v<E>) && std::constructible_from<E, ChildE>
    {
        using child_task = Task<ChildT, ChildE, void>;
        return detail::OrFailTaskAwait<TaskPromiseObject, E, child_task>{std::move(wrapped.inner).operator co_await()};
    }

    /// Pass-through for all other awaitables.
    template <typename Awaitable>
    decltype(auto) await_transform(Awaitable &&awaitable) noexcept {
        return std::forward<Awaitable>(awaitable);
    }

    TaskPromiseObject() { this->address = handle().address(); }
};

template <typename T, typename E>
struct TaskReturnObject {
    using promise_type = TaskPromiseObject<T, E>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    coroutine_handle handle;

    operator Task<T, E, void>() & noexcept;

    operator Task<T, E, void>() && noexcept;

    operator Task<T, E, Cancellation>() & noexcept;

    operator Task<T, E, Cancellation>() && noexcept;
};

template <typename T, typename E, typename C>
struct Task {
    friend struct EventLoop;
    template <typename, typename, typename>
    friend struct Task;
    template <typename, typename, typename>
    friend struct detail::OrFailTaskAwait;

    static_assert(std::is_void_v<C> || std::same_as<C, Cancellation>, "Task only supports void or Cancellation cancel channels");

    using value_type = T;
    using error_type = E;
    using cancel_type = C;

    using promise_type = TaskPromiseObject<T, E>;

    using coroutine_handle = std::coroutine_handle<promise_type>;
    using result_type = std::conditional_t<std::is_void_v<E> && std::is_void_v<C>, std::conditional_t<std::is_void_v<T>, std::nullopt_t, T>,
                                           Outcome<T, E, C>>;
    using value_result_type = std::conditional_t<std::is_void_v<E>, std::conditional_t<std::is_void_v<T>, std::nullopt_t, std::optional<T>>,
                                                 std::optional<Outcome<T, E, void>>>;

    struct Awaiter {
        Task awaitee;

        bool await_ready() noexcept { return false; }

        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> h, std::source_location location = std::source_location::current()) noexcept {
            return awaitee.h.promise().start(h.promise(), location);
        }

        auto await_resume() {
            auto &promise = awaitee.h.promise();
            promise.rethrow_if_exception();

            if constexpr (std::is_void_v<E> && std::is_void_v<C>) {
                if (promise.state != AsyncNode::FINISHED) {
                    std::abort();
                }
                if constexpr (!std::is_void_v<T>) {
                    contract_assert(promise.value.has_value());
                    return std::move(**promise.value);
                }
            } else {
                using R = Outcome<T, E, C>;

                if (promise.state == AsyncNode::CANCELLED) {
                    if constexpr (!std::is_void_v<C>) {
                        return R(outcome_cancel(C{}));
                    } else {
                        std::abort();
                    }
                }

                if constexpr (std::is_void_v<E>) {
                    contract_assert(promise.state == AsyncNode::FINISHED);
                } else {
                    contract_assert(promise.state == AsyncNode::FINISHED || promise.state == AsyncNode::FAILED);
                }
                contract_assert(promise.value.has_value());

                if constexpr (!std::is_void_v<E>) {
                    if (promise.value->has_error()) {
                        return R(outcome_error(std::move(*promise.value).error()));
                    }
                }

                if constexpr (!std::is_void_v<T>) {
                    return R(std::move(**promise.value));
                } else {
                    return R();
                }
            }
        }
    };

    auto operator co_await() && noexcept { return Awaiter(std::move(*this)); }

    /// Wrap this Task so that co_await propagates errors directly to the parent
    /// without resuming the parent coroutine. On success, returns the unwrapped value.
    ///
    ///   auto val = co_await some_task().or_fail();
    ///
    auto or_fail() && noexcept
        requires(!std::is_void_v<E>) && std::is_void_v<C>
    {
        return detail::OrFailProxy<Task>{std::move(*this)};
    }

    Task() = default;

    explicit Task(coroutine_handle h) noexcept : h(h) {
        if constexpr (!std::is_void_v<C>) {
            this->h.promise().intercept_cancel();
        }
    }

    Task(const Task &) = delete;

    Task(Task &&other) noexcept : h(other.h) { other.h = nullptr; }

    Task &operator=(const Task &) = delete;

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (h) {
                h.destroy();
            }
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (h) {
            h.destroy();
        }
    }

    [[nodiscard]] result_type result()
        pre(h != nullptr && h.promise().is_terminal() && (!h.promise().is_cancelled() || !std::is_void_v<C>) ) {
        auto &&promise = h.promise();
        promise.rethrow_if_exception();
        if constexpr (std::is_void_v<E> && std::is_void_v<C>) {
            if constexpr (!std::is_void_v<T>) {
                contract_assert(promise.value.has_value());
                return std::move(**promise.value);
            } else {
                return std::nullopt;
            }
        } else if constexpr (std::is_void_v<C>) {
            contract_assert(promise.value.has_value());
            return std::move(*promise.value);
        } else {
            return take_outcome(promise);
        }
    }

    [[nodiscard]] value_result_type value() pre(h != nullptr && h.promise().is_terminal()) {
        auto &&promise = h.promise();
        promise.rethrow_if_exception();
        if constexpr (std::is_void_v<E>) {
            if constexpr (!std::is_void_v<T>) {
                if (promise.value.has_value()) {
                    return std::optional<T>(std::move(**promise.value));
                }
                return std::optional<T>();
            } else {
                return std::nullopt;
            }
        } else {
            return std::move(promise.value);
        }
    }

    void release() { this->h = nullptr; }

    AsyncNode *operator->() pre(h != nullptr) { return &h.promise(); }

    /// Adds Cancellation interception. Idempotent if already intercepting.
    Task<T, E, Cancellation> catch_cancel() && pre(h != nullptr) {
        if constexpr (std::same_as<C, Cancellation>) {
            return std::move(*this);
        } else {
            h.promise().intercept_cancel();
            auto handle = h;
            h = nullptr;
            using target = Task<T, E, Cancellation>;
            using target_handle = typename target::coroutine_handle;
            return target(target_handle::from_address(handle.address()));
        }
    }

private:
    static auto take_outcome(promise_type &promise) {
        using R = Outcome<T, E, Cancellation>;

        if (promise.state == AsyncNode::CANCELLED) {
            return R(outcome_cancel(Cancellation{}));
        }

        if constexpr (std::is_void_v<E>) {
            contract_assert(promise.state == AsyncNode::FINISHED);
        } else {
            contract_assert(promise.state == AsyncNode::FINISHED || promise.state == AsyncNode::FAILED);
        }
        contract_assert(promise.value.has_value());

        if constexpr (!std::is_void_v<E>) {
            if (promise.value->has_error()) {
                return R(outcome_error(std::move(*promise.value).error()));
            }
        }

        if constexpr (!std::is_void_v<T>) {
            return R(std::move(**promise.value));
        } else {
            return R();
        }
    }

    coroutine_handle h;
};

template <typename T, typename E>
TaskReturnObject<T, E>::operator Task<T, E, void>() & noexcept {
    auto out = Task<T, E, void>(handle);
    handle = nullptr;
    return out;
}

template <typename T, typename E>
TaskReturnObject<T, E>::operator Task<T, E, void>() && noexcept {
    auto out = Task<T, E, void>(handle);
    handle = nullptr;
    return out;
}

template <typename T, typename E>
TaskReturnObject<T, E>::operator Task<T, E, Cancellation>() & noexcept {
    handle.promise().intercept_cancel();
    auto out = Task<T, E, Cancellation>(handle);
    handle = nullptr;
    return out;
}

template <typename T, typename E>
TaskReturnObject<T, E>::operator Task<T, E, Cancellation>() && noexcept {
    handle.promise().intercept_cancel();
    auto out = Task<T, E, Cancellation>(handle);
    handle = nullptr;
    return out;
}

} // namespace lighter
