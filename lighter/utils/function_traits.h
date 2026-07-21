#pragma once

#include <tuple>
#include <type_traits>
#include <lighter/scalar_types.hpp>

namespace lighter {

/// Check if T is a Function pointer type
template <typename T>
constexpr inline bool k_is_function_pointer_v = std::is_function_v<std::remove_pointer_t<T>>;

/// Check if T is a functor type(has a unique operator())
template <typename T, typename U = void>
constexpr bool k_is_functor_v = false;

template <typename T>
constexpr inline bool k_is_functor_v<T, std::void_t<decltype(&T::operator())>> = true;

template <typename T, typename Tuple>
struct TuplePushFront;

template <typename T, typename... Ts>
struct TuplePushFront<T, std::tuple<Ts...>> {
    using type = std::tuple<T, Ts...>;
};

template <typename T, typename Tuple>
using tuple_push_front_t = typename TuplePushFront<T, Tuple>::type;

// traits for Function types
template <typename Fn>
struct FunctionTraits;

#define FUNCTION_TRAITS_SPECIALIZE(...)                      \
    template <typename R, typename... Args>                  \
    struct FunctionTraits<R(Args...) __VA_ARGS__> {          \
        using return_type = R;                               \
        using args_type = std::tuple<Args...>;               \
        constexpr static usize args_count = sizeof...(Args); \
    };

FUNCTION_TRAITS_SPECIALIZE()
FUNCTION_TRAITS_SPECIALIZE(&)
FUNCTION_TRAITS_SPECIALIZE(const)
FUNCTION_TRAITS_SPECIALIZE(const &)
FUNCTION_TRAITS_SPECIALIZE(noexcept)
FUNCTION_TRAITS_SPECIALIZE(& noexcept)
FUNCTION_TRAITS_SPECIALIZE(const noexcept)
FUNCTION_TRAITS_SPECIALIZE(const & noexcept)

#undef FUNCTION_TRAITS_SPECIALIZE

template <typename T>
using function_return_t = typename FunctionTraits<T>::return_type;

template <typename T>
using function_args_t = typename FunctionTraits<T>::args_type;

template <typename T>
constexpr usize k_function_args_count = FunctionTraits<T>::args_count;

// traits for member pointers
template <typename T>
struct MemberTraits;

template <typename M, typename C>
struct MemberTraits<M C::*> {
    using member_type = M;
    using class_type = C;
};

template <typename T>
using member_type_t = typename MemberTraits<T>::member_type;

template <typename T>
using class_type_t = typename MemberTraits<T>::class_type;

// some traits for distinguishing between Function pointers, member Function pointers and
// functors
template <typename T, typename SFINAE = void>
struct CallableTraits;

template <typename T>
struct CallableTraits<T, std::enable_if_t<std::is_member_function_pointer_v<T>>> {
    using args_type = tuple_push_front_t<class_type_t<T> &, function_args_t<member_type_t<T>>>;
    using return_type = function_return_t<member_type_t<T>>;
};

template <typename T>
struct CallableTraits<T, std::enable_if_t<k_is_function_pointer_v<T>>> {
    using args_type = function_args_t<std::remove_pointer_t<T>>;
    using return_type = function_return_t<std::remove_pointer_t<T>>;
};

template <typename T>
struct CallableTraits<T, std::enable_if_t<k_is_functor_v<T>>> {
    using args_type = function_args_t<member_type_t<decltype(&T::operator())>>;
    using return_type = function_return_t<member_type_t<decltype(&T::operator())>>;
};

template <typename Callable>
using callable_args_t = typename CallableTraits<Callable>::args_type;

template <typename Callable>
using callable_return_t = typename CallableTraits<Callable>::return_type;

template <typename Callable>
constexpr usize k_callable_args_count_v = std::tuple_size_v<callable_args_t<Callable>>;

} // namespace lighter
