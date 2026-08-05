#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
        if (!actions.empty()) {
            auto action = std::move(actions.front());
            actions.pop_front();
            if constexpr (std::is_void_v<R>) {
                action(std::forward<Args>(args)...);
                return;
            } else {
                return action(std::forward<Args>(args)...);
            }
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

    [[nodiscard]] std::optional<std::string> verification_error() const {
        if (expected_calls && call_count != *expected_calls) {
            return call_count_message(member, *expected_calls, call_count);
        }
        return std::nullopt;
    }

    void expect() {
        if (!expected_calls) {
            expected_calls = 1;
            inferred_count = true;
        }
    }

    void allow() {
        expected_calls.reset();
        inferred_count = false;
    }

    void add_action(std::move_only_function<R(Args...)> action) {
        actions.push_back(std::move(action));
        if (inferred_count) {
            expected_calls = actions.size();
        }
    }

    std::string_view member;
    std::move_only_function<R(Args...)> behavior;
    std::deque<std::move_only_function<R(Args...)>> actions;
    std::optional<usize> expected_calls;
    usize call_count = 0;
    bool inferred_count = false;
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
        return *this;
    }

    template <typename F>
        requires std::is_invocable_r_v<R, F &, Args...>
    Expectation &then_calls(F &&implementation) {
        state.add_action(std::move_only_function<R(Args...)>(std::forward<F>(implementation)));
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

    template <typename Value>
        requires(!std::is_void_v<R> && !std::is_reference_v<R> && std::constructible_from<R, Value>)
    Expectation &then_returns(Value &&value) {
        R result(std::forward<Value>(value));
        return then_calls([result = std::move(result)](Args...) mutable -> R { return std::move(result); });
    }

    Expectation &then_returns()
        requires std::is_void_v<R>
    {
        return then_calls([](Args...) {});
    }

    Expectation &times(usize count) {
        state.expected_calls = count;
        state.inferred_count = false;
        return *this;
    }

    Expectation &once() { return times(1); }
    Expectation &never() { return times(0); }

    [[nodiscard]] usize call_count() const noexcept { return state.call_count; }

private:
    template <typename>
    friend struct Mock;

    explicit Expectation(std::shared_ptr<detail::State<R(Args...)>> state) : owner(std::move(state)), state(*owner) {}

    std::shared_ptr<detail::State<R(Args...)>> owner;
    detail::State<R(Args...)> &state;
};

/// Reflection-driven mock for a callable port.
///
/// Port must be default-constructible and all of its direct, public data
/// members must be std::copyable_function<R(Args...)> (optionally const).
/// Handles copied from Mock share the controller's state and may outlive it.
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
            port.[:member:] = [state](auto &&...args) -> decltype(auto) { return state->invoke(decltype(args)(args)...); };
        }
    }

    Mock(const Mock &) = delete;
    Mock(Mock &&) noexcept = default;
    Mock &operator=(const Mock &) = delete;
    Mock &operator=(Mock &&) noexcept = default;

    [[nodiscard]] Port handle() const { return port; }

    template <std::meta::info Member>
    [[nodiscard]] auto expect() {
        static_assert(contains<Member>(), "the reflected entity is not a direct public member of this mock's port");
        auto state = state_for<Member>();
        state->expect();
        return Expectation<detail::member_signature_t<Member>>(std::move(state));
    }

    template <std::meta::info Member>
    [[nodiscard]] auto allow() {
        static_assert(contains<Member>(), "the reflected entity is not a direct public member of this mock's port");
        auto state = state_for<Member>();
        state->allow();
        return Expectation<detail::member_signature_t<Member>>(std::move(state));
    }

    void verify() const {
        std::vector<std::string> failures;
        template for (constexpr auto member : members()) {
            if (auto failure = state_for<member>()->verification_error()) {
                failures.push_back(std::move(*failure));
            }
        }
        if (failures.empty()) {
            return;
        }
        std::string message = "mock verification failed:";
        for (const auto &failure : failures) {
            message += "\n  " + failure;
        }
        throw Error(std::move(message));
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
    [[nodiscard]] std::shared_ptr<detail::member_state_t<Member>> state_for() {
        return std::static_pointer_cast<detail::member_state_t<Member>>(states[index_of<Member>()]);
    }

    template <std::meta::info Member>
    [[nodiscard]] std::shared_ptr<const detail::member_state_t<Member>> state_for() const {
        return std::static_pointer_cast<const detail::member_state_t<Member>>(states[index_of<Member>()]);
    }

    Port port{};
    std::array<std::shared_ptr<void>, members().size()> states;
};

} // namespace lighter::mock
