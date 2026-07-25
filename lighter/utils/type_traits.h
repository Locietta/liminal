#pragma once

#include <concepts>
#include <expected>
#include <format>
#include <meta>
#include <optional>
#include <type_traits>

namespace lighter {

template <typename T>
constexpr inline bool k_dependent_false = false;

template <typename T, typename... Ts>
concept is_one_of = (std::same_as<T, Ts> || ...);

/// True when `type` names a specialization of the template `tmpl` (both as
/// reflections). Unlike the classic pattern-match formulation this handles
/// any template head uniformly, including mixed type/non-type parameters
/// (e.g. SmallVector<T, N>).
consteval bool specializes(std::meta::info type, std::meta::info tmpl) {
    return std::meta::has_template_arguments(type) && std::meta::template_of(type) == tmpl;
}

template <template <typename...> typename Template, typename T>
constexpr inline bool is_specialization_of = specializes(^^T, ^^Template);

template <typename T>
concept Formattable = std::formattable<T, char>;

template <typename L, typename R>
concept eq_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs == rhs } -> std::convertible_to<bool>;
};

template <typename L, typename R>
concept ne_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs != rhs } -> std::convertible_to<bool>;
};

template <typename L, typename R>
concept lt_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs < rhs } -> std::convertible_to<bool>;
};

template <typename L, typename R>
concept le_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs <= rhs } -> std::convertible_to<bool>;
};

template <typename L, typename R>
concept gt_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs > rhs } -> std::convertible_to<bool>;
};

template <typename L, typename R>
concept ge_comparable_with = requires(const L &lhs, const R &rhs) {
    { lhs >= rhs } -> std::convertible_to<bool>;
};

template <typename T>
constexpr inline bool is_optional_v = is_specialization_of<std::optional, std::remove_cvref_t<T>>;

template <typename T>
constexpr inline bool is_expected_v = is_specialization_of<std::expected, std::remove_cvref_t<T>>;

} // namespace lighter
