#pragma once

#include <cassert>
#include <concepts>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/utils/config.h>
#include <lighter/utils/type_list.h>
#include <lighter/utils/type_traits.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/awaitable.h>
#include <lighter/async/vocab/outcome.h>

namespace lighter::detail {

template <typename T>
constexpr inline bool k_is_task_v = k_is_specialization_of<Task, std::remove_cvref_t<T>>;

template <typename T>
using normalized_await_result_t = await_result_t<std::remove_cvref_t<T> &&>;

template <typename T, typename = void>
struct NormalizedTask;

template <typename T>
struct NormalizedTask<T, std::enable_if_t<k_is_task_v<T>>> {
    using type = std::remove_cvref_t<T>;
};

template <typename T>
struct NormalizedTask<T, std::enable_if_t<!k_is_task_v<T> && awaitable<std::remove_cvref_t<T> &&>>> {
    using type = Task<normalized_await_result_t<T>>;
};

template <typename T>
using normalized_task_t = typename NormalizedTask<T>::type;

template <typename T, typename E, typename C>
Task<T, E, C> normalize_task(Task<T, E, C> &&t) {
    return std::move(t);
}

template <typename Awaitable>
    requires(!k_is_task_v<Awaitable>) && (!std::is_reference_v<Awaitable>) &&
            std::constructible_from<std::remove_cvref_t<Awaitable>, Awaitable &&> && awaitable<std::remove_cvref_t<Awaitable> &&>
auto normalize_task_impl(std::remove_cvref_t<Awaitable> value) -> Task<normalized_await_result_t<Awaitable>> {
    if constexpr (!std::is_void_v<normalized_await_result_t<Awaitable>>) {
        co_return co_await std::move(value);
    } else {
        co_await std::move(value);
    }
}

template <typename Awaitable>
    requires(!k_is_task_v<Awaitable>) && (!std::is_reference_v<Awaitable>) &&
            std::constructible_from<std::remove_cvref_t<Awaitable>, Awaitable &&> && awaitable<std::remove_cvref_t<Awaitable> &&>
auto normalize_task(Awaitable &&input) -> Task<normalized_await_result_t<Awaitable>> {
    return normalize_task_impl<Awaitable>(std::remove_cvref_t<Awaitable>(std::forward<Awaitable>(input)));
}

template <typename T, typename E, typename C>
AsyncNode *node_from(Task<T, E, C> &t) {
    return t.operator->();
}

template <typename TaskT>
using task_error_type_t = typename TaskT::error_type;

template <typename TaskT>
using task_cancel_type_t = typename TaskT::cancel_type;

template <typename T>
struct KeepNonVoid : std::bool_constant<!std::is_void_v<T>> {};

template <typename... Ts>
using aggregated_channel_t = typename TypeListToUnion<type_list_unique_t<type_list_filter_t<TypeList<Ts...>, KeepNonVoid>>>::type;

template <typename T>
using promote_void_cancel_t = std::conditional_t<std::is_void_v<T>, Cancellation, T>;

template <typename... Ts>
constexpr inline bool k_any_non_void_v = (!std::is_void_v<Ts> || ...);

template <typename... Ts>
using aggregated_cancel_t = std::conditional_t<k_any_non_void_v<Ts...>, aggregated_channel_t<promote_void_cancel_t<Ts>...>, void>;

template <typename... Ts>
using task_group_error_type_t = typename TypeListToUnion<type_list_unique_t<TypeList<Ts...>>>::type;

template <typename Task>
using task_result_t = decltype(std::declval<Task &>().result());

template <typename Success, typename E, typename C>
using aggregate_result_t = std::conditional_t<std::is_void_v<E> && std::is_void_v<C>, Success, Outcome<Success, E, C>>;

template <bool CaptureCancel, typename Result>
auto strip_channels_from_result(Result &&result) {
    return std::forward<Result>(result);
}

template <bool CaptureCancel, typename T, typename E, typename C>
auto strip_channels_from_result(Outcome<T, E, C> &&result) {
    using type = std::conditional_t<std::is_void_v<C> || CaptureCancel, std::conditional_t<std::is_void_v<T>, std::nullopt_t, T>,
                                    Outcome<T, void, C>>;

    if constexpr (!std::is_void_v<E>) {
        assert(!result.has_error());
    }

    if constexpr (!std::is_void_v<C>) {
        if constexpr (!CaptureCancel) {
            if (result.is_cancelled()) {
                return type(outcome_cancel(std::move(result).cancellation()));
            }
        } else {
            assert(!result.is_cancelled());
        }
    }

    if constexpr (std::is_void_v<T>) {
        if constexpr (std::is_void_v<C> || CaptureCancel) {
            return std::nullopt;
        } else {
            return type();
        }
    } else {
        if constexpr (std::is_void_v<C> || CaptureCancel) {
            return std::move(*result);
        } else {
            return type(std::move(*result));
        }
    }
}

template <typename TaskT, bool CaptureCancel>
using task_success_t = decltype(strip_channels_from_result<CaptureCancel>(std::declval<task_result_t<TaskT>>()));

template <typename TaskT>
auto take_result(TaskT &task) {
    return task.result();
}

template <bool CaptureCancel, typename TaskT>
auto take_success_result(TaskT &task) {
    return strip_channels_from_result<CaptureCancel>(take_result(task));
}

template <typename TaskT>
struct RangeTasks {
    using task_type = TaskT;
};

template <typename Range>
using range_async_value_t = std::ranges::range_value_t<Range>;

template <typename Range>
using normalized_range_task_t = normalized_task_t<range_async_value_t<Range>>;

template <typename Range>
concept async_range = std::ranges::input_range<Range> && awaitable<range_async_value_t<Range>>;

template <typename Return, usize I = 0, typename Tuple, typename F>
Return tuple_visit_at_return(usize index, Tuple &tuple, F &&f) {
    if constexpr (I < std::tuple_size_v<std::remove_reference_t<Tuple>>) {
        if (index == I) {
            return f(std::integral_constant<usize, I>{}, std::get<I>(tuple));
        }
        return tuple_visit_at_return<Return, I + 1>(index, tuple, std::forward<F>(f));
    } else {
        assert(false && "tuple_visit_at_return index out of bounds");
        std::abort();
    }
}

[[noreturn]] inline void fail_empty_when_any_range() {
#if LIGHTER_ENABLE_EXCEPTIONS
    throw std::invalid_argument("WhenAny(range) requires a non-empty range");
#else
    assert(false && "WhenAny(range) requires a non-empty range");
    LIGHTER_THROW(std::invalid_argument("WhenAny(range) requires a non-empty range"));
#endif
}

} // namespace lighter::detail
