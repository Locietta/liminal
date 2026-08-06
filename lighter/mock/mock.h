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
#include <tuple>
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
concept FacadeContract = requires {
    typename T::convention_types;
    typename T::reflection_types;
};

template <typename Convention, typename Overloads = typename Convention::overload_types>
struct ConventionTraits;

struct AccessorProbe;

template <typename Convention, typename... Overloads>
struct ConventionTraits<Convention, std::tuple<Overloads...>> {
    using dispatch_type = typename Convention::dispatch_type;
    using accessor_type = typename dispatch_type::template accessor<AccessorProbe, dispatch_type, Overloads...>;
};

template <typename Convention>
using convention_dispatch_t = typename ConventionTraits<Convention>::dispatch_type;

template <typename Convention>
consteval std::meta::info convention_method() {
    if constexpr (Convention::is_direct) {
        throw "lighter::mock::Mock supports only indirect facade conventions";
    }

    using Accessor = ConventionTraits<Convention>::accessor_type;
    std::vector<std::meta::info> methods;
    for (auto member : std::meta::members_of(^^Accessor, std::meta::access_context::unchecked())) {
        if (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            methods.push_back(member);
        }
    }
    if (methods.size() != 1) {
        throw "lighter::mock::Mock facade conventions must contain one non-overloaded member dispatch";
    }
    return methods.front();
}

template <typename Contract>
consteval std::vector<std::meta::info> methods() {
    std::vector<std::meta::info> result;
    if constexpr (FacadeContract<Contract>) {
        using Conventions = typename Contract::convention_types;
        template for (constexpr auto convention :
                      std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^Conventions)))) {
            using Convention = [:convention:];
            result.push_back(convention_method<Convention>());
        }
    } else {
        for (auto member : std::meta::members_of(^^Contract, std::meta::access_context::unprivileged())) {
            if (std::meta::is_function(member) && std::meta::has_identifier(member)) {
                result.push_back(member);
            }
        }
    }
    return result;
}

template <typename Contract, std::meta::info Operation>
consteval std::meta::info resolve_method() {
    if constexpr (FacadeContract<Contract>) {
        static_assert(std::meta::is_type(Operation), "a facade mock operation must be a reflected dispatch type");
        using Conventions = typename Contract::convention_types;
        template for (constexpr auto convention :
                      std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^Conventions)))) {
            using Convention = [:convention:];
            if (std::meta::dealias(^^convention_dispatch_t<Convention>) == std::meta::dealias(Operation)) {
                return convention_method<Convention>();
            }
        }
        throw "the reflected dispatch is not an indirect convention of this mock's facade";
    } else {
        for (auto method : methods<Contract>()) {
            if (method == Operation) {
                return method;
            }
        }
        throw "the reflected entity is not a direct public method of this mock's contract";
    }
}

template <typename Contract>
consteval std::vector<std::meta::info> port_members() {
    std::vector<std::meta::info> members;
    for (auto method : methods<Contract>()) {
        if (std::meta::is_noexcept(method)) {
            throw "lighter::mock::Mock does not support noexcept functions because mock failures throw";
        }
        if (std::meta::is_volatile(method) || std::meta::is_lvalue_reference_qualified(method) ||
            std::meta::is_rvalue_reference_qualified(method)) {
            throw "lighter::mock::Mock supports only unqualified or const member functions";
        }

        const auto name = std::meta::identifier_of(method);
        for (auto member : members) {
            if (std::meta::identifier_of(member) == name) {
                throw "lighter::mock::Mock cannot represent overloaded member functions";
            }
        }

        const std::array arguments{std::meta::type_of(method)};
        members.push_back(std::meta::data_member_spec(std::meta::substitute(^^std::copyable_function, arguments), {.name = name}));
    }
    return members;
}

template <typename T>
struct MethodTraits;

template <typename R, typename... Args>
struct MethodTraits<R(Args...)> {
    using signature = R(Args...);
};

template <typename R, typename... Args>
struct MethodTraits<R(Args...) const> {
    using signature = R(Args...);
};

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

template <std::meta::info Method>
consteval std::string_view method_name() {
    return std::meta::identifier_of(Method);
}

template <std::meta::info Method>
using method_type_t = [:std::meta::type_of(Method):];

template <std::meta::info Method>
using method_signature_t = typename MethodTraits<method_type_t<Method>>::signature;

template <std::meta::info Method>
using method_state_t = State<method_signature_t<Method>>;

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

/// Reflection-driven mock for a plain method contract or ngcpp-proxy facade.
/// Operations become owning callable fields on Port. Every handle owns shared
/// dispatcher state and may outlive the controller.
template <typename Contract>
struct Mock {
    struct Port;
    consteval { std::meta::define_aggregate(^^Port, detail::port_members<Contract>()); }

    Mock() {
        template for (constexpr auto method : methods()) {
            using State = detail::method_state_t<method>;
            auto state = std::make_shared<State>();
            state->member = detail::method_name<method>();
            states[index_of<method>()] = state;
        }
    }

    Mock(const Mock &) = delete;
    Mock(Mock &&) noexcept = default;
    Mock &operator=(const Mock &) = delete;
    Mock &operator=(Mock &&) noexcept = default;

    [[nodiscard]] Port handle() const {
        Port result{};
        template for (constexpr auto method : methods()) {
            constexpr auto port_member = port_members()[index_of<method>()];
            auto state = state_for<method>();
            result.[:port_member:] = [state](auto &&...args) -> decltype(auto) { return state->invoke(decltype(args)(args)...); };
        }
        return result;
    }

    template <std::meta::info Operation>
    [[nodiscard]] auto expect() {
        constexpr auto method = detail::resolve_method<Contract, Operation>();
        auto state = state_for<method>();
        state->expect();
        return Expectation<detail::method_signature_t<method>>(std::move(state));
    }

    template <typename Dispatch>
        requires detail::FacadeContract<Contract>
    [[nodiscard]] auto expect() {
        return expect<^^Dispatch>();
    }

    template <std::meta::info Operation>
    [[nodiscard]] auto allow() {
        constexpr auto method = detail::resolve_method<Contract, Operation>();
        auto state = state_for<method>();
        state->allow();
        return Expectation<detail::method_signature_t<method>>(std::move(state));
    }

    template <typename Dispatch>
        requires detail::FacadeContract<Contract>
    [[nodiscard]] auto allow() {
        return allow<^^Dispatch>();
    }

    void verify() const {
        std::vector<std::string> failures;
        template for (constexpr auto method : methods()) {
            if (auto failure = state_for<method>()->verification_error()) {
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
    static consteval auto methods() { return std::define_static_array(detail::methods<Contract>()); }

    static consteval auto port_members() {
        return std::define_static_array(std::meta::nonstatic_data_members_of(^^Port, std::meta::access_context::unprivileged()));
    }

    template <std::meta::info Method>
    static consteval usize index_of() {
        usize index = 0;
        template for (constexpr auto candidate : methods()) {
            if (candidate == Method) {
                return index;
            }
            ++index;
        }
        throw "the reflected entity is not a method of this mock's contract";
    }

    template <std::meta::info Method>
    [[nodiscard]] std::shared_ptr<detail::method_state_t<Method>> state_for() {
        return std::static_pointer_cast<detail::method_state_t<Method>>(states[index_of<Method>()]);
    }

    template <std::meta::info Method>
    [[nodiscard]] std::shared_ptr<detail::method_state_t<Method>> state_for() const {
        return std::static_pointer_cast<detail::method_state_t<Method>>(states[index_of<Method>()]);
    }

    std::array<std::shared_ptr<void>, methods().size()> states;
};

} // namespace lighter::mock
