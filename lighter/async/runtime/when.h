#pragma once

#include <cassert>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <lighter/types.hpp>
#include <lighter/utils/memory.h>
#include <lighter/utils/small_vector.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/traits.h>
#include <lighter/async/vocab/outcome.h>

namespace lighter {

template <bool All, typename... Tasks>
struct WhenOp : AggregateOp {
private:
    constexpr static bool capture_cancel = !std::is_void_v<detail::aggregated_cancel_t<detail::task_cancel_type_t<Tasks>...>>;

public:
    using error_type = detail::aggregated_channel_t<detail::task_error_type_t<Tasks>...>;
    using cancel_type = detail::aggregated_cancel_t<detail::task_cancel_type_t<Tasks>...>;

    using success_type = std::conditional_t<All, std::tuple<detail::task_success_t<Tasks, capture_cancel>...>,
                                            std::variant<detail::task_success_t<Tasks, capture_cancel>...>>;

    using result_type = detail::aggregate_result_t<success_type, error_type, cancel_type>;

    template <detail::awaitable... U>
    explicit WhenOp(U... asyncs)
        : AggregateOp(All ? AsyncNode::NodeKind::WHEN_ALL : AsyncNode::NodeKind::WHEN_ANY),
          tasks(detail::normalize_task(std::move(asyncs))...) {
        static_assert(All || sizeof...(Tasks) > 0, "WhenAny requires at least one Task");
        if constexpr (!std::is_void_v<cancel_type>) {
            this->intercept_cancel();
        }
    }

    ~WhenOp() = default;

    bool await_ready() const noexcept {
        if constexpr (All) {
            return sizeof...(Tasks) == 0;
        } else {
            return false;
        }
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> parent_handle,
                                          std::source_location location = std::source_location::current()) noexcept {
        children.clear();
        children.reserve(sizeof...(Tasks));
        std::apply([&](auto &...ts) { (children.push_back(detail::node_from(ts)), ...); }, tasks);
        return arm_and_resume(parent_handle.promise(), location);
    }

    auto await_resume() -> result_type {
        rethrow_if_propagated();

        // Errors outrank Cancellation: settle() never reports CANCELLED when a
        // child Error was recorded, even if an external cancel raced it.
        if constexpr (!std::is_void_v<error_type>) {
            if (first_error_child != AggregateOp::k_npos) {
                auto error = detail::tuple_visit_at_return<error_type>(first_error_child, tasks, [&](auto, auto &task) -> error_type {
                    using task_t = std::remove_reference_t<decltype(task)>;
                    if constexpr (!std::is_void_v<detail::task_error_type_t<task_t>>) {
                        auto result = detail::take_result(task);
                        assert(result.has_error());
                        return error_type(std::move(result).error());
                    } else {
                        assert(false && "Error child must expose an Error channel");
                        std::abort();
                    }
                });
                return result_type(outcome_error(std::move(error)));
            }
        }

        if constexpr (!std::is_void_v<cancel_type>) {
            if (this->state == AsyncNode::CANCELLED) {
                assert(first_cancel_child != AggregateOp::k_npos && "INTERCEPT_CANCEL aggregate cancelled with no attributed child");
                auto cancel = detail::tuple_visit_at_return<cancel_type>(first_cancel_child, tasks, [&](auto, auto &task) -> cancel_type {
                    using task_t = std::remove_reference_t<decltype(task)>;
                    if constexpr (std::is_void_v<detail::task_cancel_type_t<task_t>>) {
                        return cancel_type(Cancellation{});
                    } else {
                        auto result = detail::take_result(task);
                        assert(result.is_cancelled());
                        return cancel_type(std::move(result).cancellation());
                    }
                });
                return result_type(outcome_cancel(std::move(cancel)));
            }
        }

        auto success = collect_success();
        if constexpr (std::same_as<result_type, success_type>) {
            return std::move(success);
        } else {
            return result_type(std::move(success));
        }
    }

private:
    auto collect_success() -> success_type {
        if constexpr (All) {
            return [this]<usize... I>(std::index_sequence<I...>) {
                return success_type(detail::take_success_result<capture_cancel>(std::get<I>(tasks))...);
            }(std::index_sequence_for<Tasks...>{});
        } else {
            assert(winner != AggregateOp::k_npos && "WhenAny winner not set");
            return detail::tuple_visit_at_return<success_type>(winner, tasks, [&](auto I, auto &task) -> success_type {
                return success_type(std::in_place_index<I.value>, detail::take_success_result<capture_cancel>(task));
            });
        }
    }

    std::tuple<Tasks...> tasks;
};

template <bool All, typename Task>
struct WhenOp<All, detail::RangeTasks<Task>> : AggregateOp {
private:
    constexpr static bool capture_cancel = !std::is_void_v<detail::task_cancel_type_t<Task>>;

public:
    using error_type = detail::task_error_type_t<Task>;
    using cancel_type = detail::task_cancel_type_t<Task>;

    using success_type = std::conditional_t<All, SmallVector<detail::task_success_t<Task, capture_cancel>>,
                                            std::pair<usize, detail::task_success_t<Task, capture_cancel>>>;

    using result_type = detail::aggregate_result_t<success_type, error_type, cancel_type>;

    template <detail::async_range Range>
        requires std::same_as<detail::normalized_range_task_t<Range>, Task>
    explicit WhenOp(Range range) : AggregateOp(All ? AsyncNode::NodeKind::WHEN_ALL : AsyncNode::NodeKind::WHEN_ANY) {
        if constexpr (std::ranges::sized_range<Range>) {
            tasks.reserve(std::ranges::size(range));
        }
        for (auto &&async : range) {
            tasks.emplace_back(detail::normalize_task(std::move(async)));
        }
        if constexpr (!All) {
            if (tasks.empty()) {
                detail::fail_empty_when_any_range();
            }
        }
        if constexpr (!std::is_void_v<cancel_type>) {
            this->intercept_cancel();
        }
    }

    ~WhenOp() = default;

    bool await_ready() const noexcept {
        if constexpr (All) {
            return tasks.empty();
        } else {
            return false;
        }
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> parent_handle,
                                          std::source_location location = std::source_location::current()) noexcept {
        children.clear();
        children.reserve(tasks.size());
        for (auto &task : tasks) {
            children.push_back(detail::node_from(task));
        }
        return arm_and_resume(parent_handle.promise(), location);
    }

    auto await_resume() -> result_type {
        rethrow_if_propagated();

        // Errors outrank Cancellation - see the variadic overload.
        if constexpr (!std::is_void_v<error_type>) {
            if (first_error_child != AggregateOp::k_npos) {
                auto result = detail::take_result(tasks[first_error_child]);
                assert(result.has_error());
                return result_type(outcome_error(error_type(std::move(result).error())));
            }
        }

        if constexpr (!std::is_void_v<cancel_type>) {
            if (this->state == AsyncNode::CANCELLED) {
                assert(first_cancel_child != AggregateOp::k_npos && "INTERCEPT_CANCEL aggregate cancelled with no attributed child");
                auto result = detail::take_result(tasks[first_cancel_child]);
                assert(result.is_cancelled());
                return result_type(outcome_cancel(std::move(result).cancellation()));
            }
        }

        auto success = collect_success();
        if constexpr (std::same_as<result_type, success_type>) {
            return std::move(success);
        } else {
            return result_type(std::move(success));
        }
    }

private:
    auto collect_success() -> success_type {
        if constexpr (All) {
            success_type results;
            results.reserve(tasks.size());
            for (auto &task : tasks) {
                results.emplace_back(detail::take_success_result<capture_cancel>(task));
            }
            return results;
        } else {
            assert(winner != AggregateOp::k_npos && "WhenAny winner not set");
            return success_type{winner, detail::take_success_result<capture_cancel>(tasks[winner])};
        }
    }

    SmallVector<Task> tasks;
};

/// Awaits all tasks concurrently, collecting results into a tuple.
///
/// Variadic overload: accepts heterogeneous awaitables (tasks, Semaphore acquires, etc.)
/// and returns `std::tuple<T...>` where each element is the result of the corresponding Task.
/// Void tasks produce `std::nullopt_t` in the tuple.
///
/// Range overload: accepts a range of homogeneous tasks and returns `SmallVector<T>`.
///
/// If any child Task produces a structured Error, the first Error cancels all siblings
/// and the combinator returns `Outcome<..., E, ...>` carrying that Error.
/// If any child cancels (via `co_await cancel()` or an external token), Cancellation
/// propagates to all siblings and the combinator returns the Cancellation.
/// Errors take priority over cancellations.
///
/// All children are guaranteed to have completed before the aggregate returns
/// (structured completion).
///
/// Accepts any awaitable that satisfies the `awaitable` concept, including synchronous
/// awaiters like `Semaphore::AcquireAwaiter`.
template <typename... Tasks>
struct WhenAll : private WhenOp<true, Tasks...> {
private:
    using WhenOp<true, Tasks...>::WhenOp;

public:
    using WhenOp<true, Tasks...>::await_ready;
    using WhenOp<true, Tasks...>::await_suspend;
    using WhenOp<true, Tasks...>::await_resume;
};

/// Awaits multiple tasks concurrently, returning the first to complete.
///
/// Variadic overload: accepts heterogeneous awaitables and returns
/// `std::variant<T...>` where the active alternative corresponds to the winning Task.
/// Void tasks produce `std::nullopt_t` in the variant.
///
/// Range overload: accepts a range of homogeneous tasks and returns
/// `std::pair<usize, T>` where the first element is the index of the winner.
///
/// Once a winner is determined, all remaining tasks are cancelled.
/// If the first-to-complete Task produces a structured Error, the combinator
/// returns `Outcome<..., E, ...>` carrying that Error.
/// If the first-to-complete Task cancels, Cancellation propagates to the parent.
///
/// Requires at least one task; `WhenAny<>` is explicitly deleted.
///
/// All siblings are guaranteed to have completed before the aggregate returns
/// (structured completion).
///
/// Accepts any awaitable that satisfies the `awaitable` concept, including synchronous
/// awaiters like `Semaphore::AcquireAwaiter`.
template <typename... Tasks>
struct WhenAny : private WhenOp<false, Tasks...> {
private:
    using WhenOp<false, Tasks...>::WhenOp;

public:
    using WhenOp<false, Tasks...>::await_ready;
    using WhenOp<false, Tasks...>::await_suspend;
    using WhenOp<false, Tasks...>::await_resume;
};

template <>
struct WhenAny<> {
    WhenAny() = delete;
};

template <detail::awaitable... Tasks>
WhenAll(Tasks...) -> WhenAll<detail::normalized_task_t<Tasks>...>;

template <detail::async_range Range>
WhenAll(Range) -> WhenAll<detail::RangeTasks<detail::normalized_range_task_t<Range>>>;

template <detail::awaitable... Tasks>
    requires(sizeof...(Tasks) > 0)
WhenAny(Tasks...) -> WhenAny<detail::normalized_task_t<Tasks>...>;

template <detail::async_range Range>
WhenAny(Range) -> WhenAny<detail::RangeTasks<detail::normalized_range_task_t<Range>>>;

template <detail::awaitable... Tasks>
auto when_all(Tasks &&...tasks) {
    return WhenAll(std::forward<Tasks>(tasks)...);
}

template <detail::async_range Range>
auto when_all(Range &&range) {
    return WhenAll(std::forward<Range>(range));
}

template <detail::awaitable... Tasks>
    requires(sizeof...(Tasks) > 0)
auto when_any(Tasks &&...tasks) {
    return WhenAny(std::forward<Tasks>(tasks)...);
}

template <detail::async_range Range>
auto when_any(Range &&range) {
    return WhenAny(std::forward<Range>(range));
}

} // namespace lighter
