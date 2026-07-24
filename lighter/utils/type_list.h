#pragma once

#include <concepts>
#include <type_traits>
#include <variant>

#include <lighter/types.hpp>

namespace lighter {

template <typename T, typename... Ts>
concept is_one_of = (std::same_as<T, Ts> || ...);

template <typename... Ts>
struct TypeList {};

template <typename List, typename T>
struct TypeListPrepend;

template <typename... Ts, typename T>
struct TypeListPrepend<TypeList<Ts...>, T> {
    using type = TypeList<T, Ts...>;
};

template <typename List, typename T>
using type_list_prepend_t = typename TypeListPrepend<List, T>::type;

template <typename List, typename T>
struct TypeListContains;

template <typename... Ts, typename T>
struct TypeListContains<TypeList<Ts...>, T> : std::bool_constant<(std::same_as<T, Ts> || ...)> {};

template <typename List, typename T>
constexpr inline bool type_list_contains_v = TypeListContains<List, T>::value;

template <typename List, template <typename> typename Predicate>
struct TypeListFilter;

template <template <typename> typename Predicate>
struct TypeListFilter<TypeList<>, Predicate> {
    using type = TypeList<>;
};

template <typename T, typename... Ts, template <typename> typename Predicate>
struct TypeListFilter<TypeList<T, Ts...>, Predicate> {
private:
    using tail = typename TypeListFilter<TypeList<Ts...>, Predicate>::type;

public:
    using type = std::conditional_t<Predicate<T>::value, type_list_prepend_t<tail, T>, tail>;
};

template <typename List, template <typename> typename Predicate>
using type_list_filter_t = typename TypeListFilter<List, Predicate>::type;

// Applies Function directly to each element (works with class and alias
// templates alike), unlike Filter's Predicate which is read through ::value.
template <typename List, template <typename> typename Function>
struct TypeListTransform;

template <typename... Ts, template <typename> typename Function>
struct TypeListTransform<TypeList<Ts...>, Function> {
    using type = TypeList<Function<Ts>...>;
};

template <typename List, template <typename> typename Function>
using type_list_transform_t = typename TypeListTransform<List, Function>::type;

template <typename List>
struct TypeListUnique;

template <typename Accum, typename List>
struct TypeListUniqueImpl;

template <>
struct TypeListUnique<TypeList<>> {
    using type = TypeList<>;
};

template <typename... Ts>
struct TypeListUniqueImpl<TypeList<Ts...>, TypeList<>> {
    using type = TypeList<Ts...>;
};

template <typename... Ts, typename T, typename... Rest>
struct TypeListUniqueImpl<TypeList<Ts...>, TypeList<T, Rest...>> {
private:
    using next = std::conditional_t<type_list_contains_v<TypeList<Ts...>, T>, TypeList<Ts...>, TypeList<Ts..., T>>;

public:
    using type = typename TypeListUniqueImpl<next, TypeList<Rest...>>::type;
};

template <typename... Ts>
struct TypeListUnique<TypeList<Ts...>> {
    using type = typename TypeListUniqueImpl<TypeList<>, TypeList<Ts...>>::type;
};

template <typename List>
using type_list_unique_t = typename TypeListUnique<List>::type;

template <usize I, typename List>
struct TypeListElement;

template <usize I, typename First, typename... Rest>
struct TypeListElement<I, TypeList<First, Rest...>> : TypeListElement<I - 1, TypeList<Rest...>> {};

template <typename First, typename... Rest>
struct TypeListElement<0, TypeList<First, Rest...>> {
    using type = First;
};

template <usize I, typename List>
using type_list_element_t = typename TypeListElement<I, List>::type;

// Index of the first occurrence of T; a list that does not contain T is a
// compile error (use TypeListContains to test membership first).
template <typename List, typename T>
struct TypeListIndexOf;

template <typename T, typename... Rest>
struct TypeListIndexOf<TypeList<T, Rest...>, T> : std::integral_constant<usize, 0> {};

template <typename First, typename... Rest, typename T>
struct TypeListIndexOf<TypeList<First, Rest...>, T> : std::integral_constant<usize, 1 + TypeListIndexOf<TypeList<Rest...>, T>::value> {};

template <typename List, typename T>
constexpr inline usize type_list_index_of_v = TypeListIndexOf<List, T>::value;

template <typename A, typename B>
struct TypeListCat;

template <typename... As, typename... Bs>
struct TypeListCat<TypeList<As...>, TypeList<Bs...>> {
    using type = TypeList<As..., Bs...>;
};

template <typename A, typename B>
using type_list_cat_t = typename TypeListCat<A, B>::type;

template <typename... Lists>
struct TypeListConcat;

template <>
struct TypeListConcat<> {
    using type = TypeList<>;
};

template <typename List>
struct TypeListConcat<List> {
    using type = List;
};

template <typename First, typename Second, typename... Rest>
struct TypeListConcat<First, Second, Rest...> : TypeListConcat<type_list_cat_t<First, Second>, Rest...> {};

template <typename... Lists>
using type_list_concat_t = typename TypeListConcat<Lists...>::type;

template <typename List>
struct TypeListSize;

template <typename... Ts>
struct TypeListSize<TypeList<Ts...>> : std::integral_constant<usize, sizeof...(Ts)> {};

template <typename List>
constexpr inline usize type_list_size_v = TypeListSize<List>::value;

template <typename List>
struct TypeListToUnion;

template <>
struct TypeListToUnion<TypeList<>> {
    using type = void;
};

template <typename T>
struct TypeListToUnion<TypeList<T>> {
    using type = T;
};

template <typename... Ts>
    requires(sizeof...(Ts) > 1)
struct TypeListToUnion<TypeList<Ts...>> {
    using type = std::variant<Ts...>;
};

} // namespace lighter
