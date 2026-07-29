#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include <lighter/types.hpp>
#include <lighter/utils/config.h>

namespace lighter {

template <typename T, typename E = void, typename C = void>
struct Outcome;

template <typename T>
constexpr bool is_outcome_v = false;

template <typename T, typename E, typename C>
constexpr bool is_outcome_v<Outcome<T, E, C>> = true;

struct OutcomeOkTag {};

inline OutcomeOkTag outcome_value() { return {}; }

template <typename E>
struct OutcomeError {
    E value;
};

template <typename E>
OutcomeError<std::decay_t<E>> outcome_error(E &&e) {
    return {std::forward<E>(e)};
}

template <typename C>
struct OutcomeCancel {
    C value;
};

template <typename C>
OutcomeCancel<std::decay_t<C>> outcome_cancel(C &&c) {
    return {std::forward<C>(c)};
}

template <typename T, typename E, typename C>
struct Outcome {
    using value_type = T;
    using error_type = E;
    using cancel_type = C;

    enum class State : u8 { OK, ERR, CANCELLED };

private:
    template <typename X>
    using member_t = std::conditional_t<std::is_void_v<X>, std::type_identity<void>, X>;

    // GCC 16.1 ICEs when a contract on an explicit this member function is combined
    // with a placeholder return type. Keep the return type explicitly
    // dependent while preserving the cv/ref category deduced for Self.
    // See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=115168 for the ICE report.
    template <usize I, typename Self>
    using member_ref_t = decltype(std::get<I>(std::declval<Self &&>().variant));

public:
    template <typename U = T>
        requires(!std::is_void_v<T>) && std::constructible_from<T, U &&> &&
                (!is_outcome_v<std::decay_t<U>> || std::same_as<std::decay_t<U>, T>)
    Outcome(U &&value) : variant(std::in_place_index<0>, T(std::forward<U>(value))) {}

    Outcome()
        requires std::is_void_v<T>
        : variant(std::in_place_index<0>) {}

    template <typename U>
        requires(!std::is_void_v<E>) && std::constructible_from<E, U>
    Outcome(OutcomeError<U> e) : variant(std::in_place_index<1>, E(std::move(e.value))) {}

    template <typename U>
        requires(!std::is_void_v<C>) && std::constructible_from<C, U>
    Outcome(OutcomeCancel<U> c) : variant(std::in_place_index<2>, C(std::move(c.value))) {}

    Outcome(OutcomeOkTag)
        requires std::is_void_v<T>
        : variant(std::in_place_index<0>) {}

    State state() const noexcept { return State(variant.index()); }

    bool has_value() const noexcept { return variant.index() == 0; }

    bool has_error() const noexcept
        requires(!std::is_void_v<E>)
    {
        return variant.index() == 1;
    }

    bool is_cancelled() const noexcept
        requires(!std::is_void_v<C>)
    {
        return variant.index() == 2;
    }

    explicit operator bool() const noexcept { return has_value(); }

    template <typename Self>
    member_ref_t<0, Self> value(this Self &&self)
        requires(!std::is_void_v<T>)
    pre(self.has_value()) {
        return std::get<0>(std::forward<Self>(self).variant);
    }

    template <typename Self>
    decltype(auto) operator*(this Self &&self)
        requires(!std::is_void_v<T>)
    {
        return std::forward<Self>(self).value();
    }

    auto *operator->()
        requires(!std::is_void_v<T>)
    {
        return std::addressof(value());
    }

    const auto *operator->() const
        requires(!std::is_void_v<T>)
    {
        return std::addressof(value());
    }

    template <typename Self>
    member_ref_t<1, Self> error(this Self &&self)
        requires(!std::is_void_v<E>)
    pre(self.has_error()) {
        return std::get<1>(std::forward<Self>(self).variant);
    }

    template <typename Self>
    member_ref_t<2, Self> cancellation(this Self &&self)
        requires(!std::is_void_v<C>)
    pre(self.is_cancelled()) {
        return std::get<2>(std::forward<Self>(self).variant);
    }

private:
    std::variant<member_t<T>, member_t<E>, member_t<C>> variant;
};

template <typename T>
struct Outcome<T, void, void> {
private:
    using stored_type = std::conditional_t<std::is_void_v<T>, std::type_identity<void>, T>;

public:
    using value_type = T;
    using error_type = void;
    using cancel_type = void;

    template <typename U = T>
        requires(!std::is_void_v<T>) && std::constructible_from<T, U &&> &&
                (!is_outcome_v<std::decay_t<U>> || std::same_as<std::decay_t<U>, T>)
    Outcome(U &&value) : data(T(std::forward<U>(value))) {}

    Outcome()
        requires std::is_void_v<T>
    {}

    Outcome(OutcomeOkTag)
        requires std::is_void_v<T>
    {}

    constexpr bool has_value() const noexcept { return true; }

    constexpr explicit operator bool() const noexcept { return true; }

    template <typename Self>
    decltype(auto) value(this Self &&self)
        requires(!std::is_void_v<T>)
    {
        return std::forward<Self>(self).data;
    }

    template <typename Self>
    decltype(auto) operator*(this Self &&self)
        requires(!std::is_void_v<T>)
    {
        return std::forward<Self>(self).value();
    }

    auto *operator->()
        requires(!std::is_void_v<T>)
    {
        return std::addressof(value());
    }

    const auto *operator->() const
        requires(!std::is_void_v<T>)
    {
        return std::addressof(value());
    }

private:
    LIGHTER_NO_UNIQUE_ADDRESS
    std::conditional_t<std::is_void_v<T>, std::type_identity<void>, T> data;
};

} // namespace lighter
