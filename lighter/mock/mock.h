#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <lighter/types.hpp>

namespace lighter::mock {

/// Base exception for mock configuration and verification failures.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

template <typename T>
struct CopyableFunctionTraits;

template <typename R, typename... Args>
struct CopyableFunctionTraits<std::copyable_function<R(Args...)>> {
    using signature = R(Args...);
};

template <typename R, typename... Args>
struct CopyableFunctionTraits<std::copyable_function<R(Args...) const>> {
    using signature = R(Args...);
};

template <typename T>
concept CopyableFunction = requires { typename CopyableFunctionTraits<T>::signature; };

inline std::string call_count_message(std::string_view member, usize expected, usize actual) {
    return std::string(member) + ": expected " + std::to_string(expected) + " call(s), observed " + std::to_string(actual);
}

template <typename Signature>
struct State;

template <typename R, typename... Args>
struct State<R(Args...)> {
    R invoke(Args... args) {
        ++call_count;
        if (expected_calls && call_count > *expected_calls) {
            throw Error(call_count_message(member, *expected_calls, call_count));
        }
        if (!behavior) {
            throw Error(std::string(member) + ": unexpected call (no behavior configured)");
        }

        if constexpr (std::is_void_v<R>) {
            behavior(std::forward<Args>(args)...);
        } else {
            return behavior(std::forward<Args>(args)...);
        }
    }

    void verify() const {
        if (expected_calls && call_count != *expected_calls) {
            throw Error(call_count_message(member, *expected_calls, call_count));
        }
    }

    void configure_behavior() {
        if (!expected_calls) {
            expected_calls = 1;
        }
    }

    std::string_view member;
    std::move_only_function<R(Args...)> behavior;
    std::optional<usize> expected_calls;
    usize call_count = 0;
};

template <std::meta::info Member>
consteval std::string_view member_name() {
    return std::meta::identifier_of(Member);
}

template <std::meta::info Member>
using member_type_t = [:std::meta::type_of(Member):];

template <std::meta::info Member>
using member_signature_t = typename CopyableFunctionTraits<member_type_t<Member>>::signature;

template <std::meta::info Member>
using member_state_t = State<member_signature_t<Member>>;

} // namespace detail

/// Typed configuration and observation handle for one reflected port member.
template <typename Signature>
struct Expectation;

template <typename R, typename... Args>
struct Expectation<R(Args...)> {
    template <typename F>
        requires std::is_invocable_r_v<R, F &, Args...>
    Expectation &calls(F &&implementation) {
        state.behavior = std::forward<F>(implementation);
        state.configure_behavior();
        return *this;
    }

    template <typename Value>
        requires(!std::is_void_v<R> && !std::is_reference_v<R> && std::copy_constructible<R> && std::constructible_from<R, Value>)
    Expectation &returns(Value &&value) {
        R result(std::forward<Value>(value));
        return calls([result = std::move(result)](Args...) -> R { return result; });
    }

    Expectation &returns()
        requires std::is_void_v<R>
    {
        return calls([](Args...) {});
    }

    Expectation &times(usize count) {
        state.expected_calls = count;
        return *this;
    }

    Expectation &once() { return times(1); }
    Expectation &never() { return times(0); }

    [[nodiscard]] usize call_count() const noexcept { return state.call_count; }

private:
    template <typename>
    friend struct Mock;

    explicit Expectation(detail::State<R(Args...)> &state) : state(state) {}

    detail::State<R(Args...)> &state;
};

/// Reflection-driven mock for a callable port.
///
/// Port must be default-constructible and all of its direct, public data
/// members must be std::copyable_function<R(Args...)> (optionally const).
/// Mock owns the port and must outlive every reference to object().
template <typename Port>
struct Mock {
    Mock() {
        template for (constexpr auto member : members()) {
            using MemberType = detail::member_type_t<member>;
            static_assert(detail::CopyableFunction<MemberType>, "lighter::mock::Mock port members must be std::copyable_function objects");

            using State = detail::member_state_t<member>;
            auto state = std::make_shared<State>();
            state->member = detail::member_name<member>();
            states[index_of<member>()] = state;

            port.[:member:] = [this](auto &&...args) -> decltype(auto) { return state_for<member>().invoke(decltype(args)(args)...); };
        }
    }

    Mock(const Mock &) = delete;
    Mock(Mock &&) = delete;
    Mock &operator=(const Mock &) = delete;
    Mock &operator=(Mock &&) = delete;

    [[nodiscard]] Port &object() noexcept { return port; }
    [[nodiscard]] const Port &object() const noexcept { return port; }

    template <std::meta::info Member>
    [[nodiscard]] auto on() {
        static_assert(contains<Member>(), "the reflected entity is not a direct public member of this mock's port");
        return Expectation<detail::member_signature_t<Member>>(state_for<Member>());
    }

    void verify() const {
        template for (constexpr auto member : members()) { state_for<member>().verify(); }
    }

private:
    static consteval auto members() {
        return std::define_static_array(std::meta::nonstatic_data_members_of(^^Port, std::meta::access_context::unprivileged()));
    }

    template <std::meta::info Member>
    static consteval bool contains() {
        template for (constexpr auto candidate : members()) {
            if (candidate == Member) {
                return true;
            }
        }
        return false;
    }

    template <std::meta::info Member>
    static consteval usize index_of() {
        usize index = 0;
        template for (constexpr auto candidate : members()) {
            if (candidate == Member) {
                return index;
            }
            ++index;
        }
        throw "the reflected entity is not a member of this mock's port";
    }

    template <std::meta::info Member>
    [[nodiscard]] detail::member_state_t<Member> &state_for() {
        return *static_cast<detail::member_state_t<Member> *>(states[index_of<Member>()].get());
    }

    template <std::meta::info Member>
    [[nodiscard]] const detail::member_state_t<Member> &state_for() const {
        return *static_cast<const detail::member_state_t<Member> *>(states[index_of<Member>()].get());
    }

    Port port{};
    std::array<std::shared_ptr<void>, members().size()> states;
};

} // namespace lighter::mock
