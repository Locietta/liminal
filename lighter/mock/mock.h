#pragma once

#include <array>
#include <atomic>
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

#include <proxy/proxy.h>

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
    static_assert(sizeof...(Overloads) == 1, "lighter::mock::Mock facade conventions cannot be overloaded");

    using dispatch_type = typename Convention::dispatch_type;
    using accessor_type = typename dispatch_type::template accessor<AccessorProbe, dispatch_type, Overloads...>;
    using overload_type = std::tuple_element_t<0, std::tuple<Overloads...>>;
};

template <typename Convention>
using convention_dispatch_t = typename ConventionTraits<Convention>::dispatch_type;

template <typename Convention>
consteval std::string_view convention_name() {
    using Accessor = ConventionTraits<Convention>::accessor_type;
    std::vector<std::meta::info> methods;
    for (auto member : std::meta::members_of(^^Accessor, std::meta::access_context::unchecked())) {
        if (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            methods.push_back(member);
        }
    }
    if (methods.size() == 1) {
        return std::meta::identifier_of(methods.front());
    }
    if (!methods.empty()) {
        throw "lighter::mock::Mock facade conventions cannot be overloaded";
    }
    constexpr auto dispatch = std::meta::dealias(^^convention_dispatch_t<Convention>);
    if constexpr (std::meta::has_identifier(dispatch)) {
        return std::meta::identifier_of(dispatch);
    }
    return "facade convention";
}

template <typename Contract>
consteval std::vector<std::meta::info> facade_conventions() {
    std::vector<std::meta::info> result;
    using Conventions = typename Contract::convention_types;
    for (auto convention : std::meta::template_arguments_of(std::meta::dealias(^^Conventions))) {
        result.push_back(convention);
    }
    return result;
}

template <typename Contract>
consteval std::vector<std::meta::info> methods() {
    std::vector<std::meta::info> result;
    for (auto member : std::meta::members_of(^^Contract, std::meta::access_context::unprivileged())) {
        if (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            result.push_back(member);
        }
    }
    return result;
}

template <typename Contract, std::meta::info Dispatch>
consteval std::meta::info resolve_convention() {
    static_assert(std::meta::is_type(Dispatch), "a facade mock operation must be a reflected dispatch type");
    template for (constexpr auto convention : std::define_static_array(facade_conventions<Contract>())) {
        using Convention = [:convention:];
        if (std::meta::dealias(^^convention_dispatch_t<Convention>) == std::meta::dealias(Dispatch)) {
            return convention;
        }
    }
    throw "the reflected dispatch is not a convention of this mock's facade";
}

template <typename Contract, std::meta::info Method>
consteval std::meta::info resolve_method() {
    for (auto candidate : methods<Contract>()) {
        if (candidate == Method) {
            return candidate;
        }
    }
    throw "the reflected entity is not a direct public method of this mock's contract";
}

template <typename Contract>
consteval std::vector<std::meta::info> port_members() {
    std::vector<std::meta::info> members;
    if constexpr (FacadeContract<Contract>) {
        return members;
    }
    for (auto method : methods<Contract>()) {
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

template <typename R, typename... Args>
struct MethodTraits<R(Args...) noexcept> {
    using signature = R(Args...) noexcept;
};

template <typename R, typename... Args>
struct MethodTraits<R(Args...) const noexcept> {
    using signature = R(Args...) noexcept;
};

inline std::string call_count_message(std::string_view member, usize expected, usize actual) {
    return std::string(member) + ": expected " + std::to_string(expected) + " call(s), observed " + std::to_string(actual);
}

template <typename Signature>
struct State;

template <bool IsNoexcept, typename R, typename... Args>
struct StateSignature;

template <typename R, typename... Args>
struct StateSignature<false, R, Args...> {
    using type = R(Args...);
};

template <typename R, typename... Args>
struct StateSignature<true, R, Args...> {
    using type = R(Args...) noexcept;
};

template <bool IsNoexcept, typename R, typename... Args>
struct StateBase {
    using signature = typename StateSignature<IsNoexcept, R, Args...>::type;
    using action_type = std::move_only_function<signature>;

    static_assert(!IsNoexcept || std::is_void_v<R> || std::is_nothrow_default_constructible_v<R>,
                  "noexcept mock operations must return void or a nothrow-default-constructible type");

    R invoke(Args... args) noexcept(IsNoexcept) {
        ++call_count;
        if (expected_calls && call_count > *expected_calls) {
            if constexpr (!IsNoexcept) {
                throw Error(call_count_message(member, *expected_calls, call_count));
            }
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
            if constexpr (IsNoexcept) {
                unexpected_call = true;
                if constexpr (std::is_void_v<R>) {
                    return;
                } else {
                    return R{};
                }
            } else {
                throw Error(std::string(member) + ": unexpected call (no behavior configured)");
            }
        }

        if constexpr (std::is_void_v<R>) {
            behavior(std::forward<Args>(args)...);
        } else {
            return behavior(std::forward<Args>(args)...);
        }
    }

    [[nodiscard]] std::optional<std::string> verification_error() const {
        if (unexpected_call) {
            return std::string(member) + ": unexpected call (no behavior configured)";
        }
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

    void add_action(action_type action) {
        actions.push_back(std::move(action));
        if (inferred_count) {
            expected_calls = actions.size();
        }
    }

    std::string_view member;
    action_type behavior;
    std::deque<action_type> actions;
    std::optional<usize> expected_calls;
    usize call_count = 0;
    bool inferred_count = false;
    bool unexpected_call = false;
};

template <typename R, typename... Args>
struct State<R(Args...)> : StateBase<false, R, Args...> {};

template <typename R, typename... Args>
struct State<R(Args...) noexcept> : StateBase<true, R, Args...> {};

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

template <typename Convention>
using convention_type_t = typename ConventionTraits<Convention>::overload_type;

template <typename Convention>
using convention_signature_t = typename MethodTraits<convention_type_t<Convention>>::signature;

template <typename Convention>
using convention_state_t = State<convention_signature_t<Convention>>;

template <typename Contract>
using facade_states_t = std::array<std::shared_ptr<void>, std::tuple_size_v<typename Contract::convention_types>>;

template <typename Contract, typename Convention>
consteval usize convention_index() {
    usize index = 0;
    template for (constexpr auto candidate : std::define_static_array(facade_conventions<Contract>())) {
        if (candidate == ^^Convention) {
            return index;
        }
        ++index;
    }
    throw "the convention is not part of this mock's facade";
}

template <typename Contract, typename Convention>
struct FacadeDispatch {
    template <typename Self, typename... Args>
        requires requires(Self &&self, Args &&...args) {
            std::forward<Self>(self).template invoke<Convention>(std::forward<Args>(args)...);
        }
    decltype(auto) operator()(Self &&self, Args &&...args) const
        noexcept(noexcept(std::forward<Self>(self).template invoke<Convention>(std::forward<Args>(args)...))) {
        return std::forward<Self>(self).template invoke<Convention>(std::forward<Args>(args)...);
    }
};

template <typename Dispatch, typename Overload, typename Self, typename... Args>
    requires requires(Self &&self, Args &&...args) { Dispatch{}(std::forward<Self>(self), std::forward<Args>(args)...); }
decltype(auto) invoke(Self &&self, Args &&...args) noexcept(noexcept(Dispatch{}(std::forward<Self>(self), std::forward<Args>(args)...))) {
    return Dispatch{}(std::forward<Self>(self), std::forward<Args>(args)...);
}

template <bool Enabled, typename Owner, typename Contract, typename Convention, typename Overloads = typename Convention::overload_types>
struct ConventionAccessor {};

template <typename Owner, typename Contract, typename Convention, typename... Overloads>
struct ConventionAccessor<true, Owner, Contract, Convention, std::tuple<Overloads...>>
    : Convention::dispatch_type::template accessor<Owner, FacadeDispatch<Contract, Convention>, Overloads...> {};

template <typename Owner, typename Contract, bool Direct, typename Conventions = typename Contract::convention_types>
struct FacadeAccessors;

template <typename Owner, typename Contract, bool Direct, typename... Conventions>
struct FacadeAccessors<Owner, Contract, Direct, std::tuple<Conventions...>>
    : ConventionAccessor<Conventions::is_direct == Direct, Owner, Contract, Conventions>... {};

template <typename Contract>
struct FacadePointee : FacadeAccessors<FacadePointee<Contract>, Contract, false> {
    explicit FacadePointee(facade_states_t<Contract> states) : states(std::move(states)) {}

    template <typename Convention, typename... Args>
    decltype(auto) invoke(Args &&...args) const
        noexcept(noexcept(std::declval<convention_state_t<Convention> &>().invoke(std::forward<Args>(args)...))) {
        auto state = std::static_pointer_cast<convention_state_t<Convention>>(states[convention_index<Contract, Convention>()]);
        return state->invoke(std::forward<Args>(args)...);
    }

    void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    facade_states_t<Contract> states;
    std::atomic<usize> references{1};
};

template <typename Contract>
struct FacadePointer : FacadeAccessors<FacadePointer<Contract>, Contract, true> {
    using element_type = FacadePointee<Contract>;

    explicit FacadePointer(facade_states_t<Contract> states) : pointee(new element_type(std::move(states))) {}

    FacadePointer(const FacadePointer &other) noexcept : pointee(other.pointee) {
        if (pointee) {
            pointee->retain();
        }
    }

    FacadePointer(FacadePointer &&other) noexcept : pointee(std::exchange(other.pointee, nullptr)) {}

    FacadePointer &operator=(const FacadePointer &other) noexcept {
        auto *replacement = other.pointee;
        if (replacement) {
            replacement->retain();
        }
        if (pointee) {
            pointee->release();
        }
        pointee = replacement;
        return *this;
    }

    FacadePointer &operator=(FacadePointer &&other) noexcept {
        std::swap(pointee, other.pointee);
        return *this;
    }

    ~FacadePointer() {
        if (pointee) {
            pointee->release();
        }
    }

    element_type &operator*() & noexcept { return *pointee; }
    const element_type &operator*() const & noexcept { return *pointee; }
    element_type &&operator*() && noexcept { return std::move(*pointee); }
    const element_type &&operator*() const && noexcept { return std::move(*pointee); }

    template <typename Convention, typename... Args>
    decltype(auto) invoke(Args &&...args) const noexcept(noexcept(pointee->template invoke<Convention>(std::forward<Args>(args)...))) {
        return pointee->template invoke<Convention>(std::forward<Args>(args)...);
    }

    element_type *pointee;
};

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

template <typename R, typename... Args>
struct Expectation<R(Args...) noexcept> {
    template <typename F>
        requires std::is_nothrow_invocable_r_v<R, F &, Args...>
    Expectation &calls(F &&implementation) {
        state.behavior = std::forward<F>(implementation);
        return *this;
    }

    template <typename F>
        requires std::is_nothrow_invocable_r_v<R, F &, Args...>
    Expectation &then_calls(F &&implementation) {
        state.add_action(std::move_only_function<R(Args...) noexcept>(std::forward<F>(implementation)));
        return *this;
    }

    template <typename Value>
        requires(!std::is_void_v<R> && !std::is_reference_v<R> && std::copy_constructible<R> && std::is_nothrow_copy_constructible_v<R> &&
                 std::constructible_from<R, Value>)
    Expectation &returns(Value &&value) {
        R result(std::forward<Value>(value));
        return calls([result = std::move(result)](Args...) noexcept -> R { return result; });
    }

    Expectation &returns()
        requires std::is_void_v<R>
    {
        return calls([](Args...) noexcept {});
    }

    template <typename Value>
        requires(!std::is_void_v<R> && !std::is_reference_v<R> && std::constructible_from<R, Value> &&
                 std::is_nothrow_move_constructible_v<R>)
    Expectation &then_returns(Value &&value) {
        R result(std::forward<Value>(value));
        return then_calls([result = std::move(result)](Args...) mutable noexcept -> R { return std::move(result); });
    }

    Expectation &then_returns()
        requires std::is_void_v<R>
    {
        return then_calls([](Args...) noexcept {});
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

    explicit Expectation(std::shared_ptr<detail::State<R(Args...) noexcept>> state) : owner(std::move(state)), state(*owner) {}

    std::shared_ptr<detail::State<R(Args...) noexcept>> owner;
    detail::State<R(Args...) noexcept> &state;
};

/// Reflection-driven mock for a plain method contract or ngcpp-proxy facade.
/// Every generated handle owns shared dispatcher state and may outlive the
/// controller.
template <typename Contract>
struct Mock {
    struct Port;
    consteval {
        if constexpr (!detail::FacadeContract<Contract>) {
            std::meta::define_aggregate(^^Port, detail::port_members<Contract>());
        }
    }

    Mock() {
        if constexpr (detail::FacadeContract<Contract>) {
            template for (constexpr auto convention : conventions()) {
                using Convention = [:convention:];
                using State = detail::convention_state_t<Convention>;
                auto state = std::make_shared<State>();
                state->member = detail::convention_name<Convention>();
                states[detail::convention_index<Contract, Convention>()] = state;
            }
        } else {
            template for (constexpr auto method : methods()) {
                using State = detail::method_state_t<method>;
                auto state = std::make_shared<State>();
                state->member = detail::method_name<method>();
                states[index_of<method>()] = state;
            }
        }
    }

    Mock(const Mock &) = delete;
    Mock(Mock &&) noexcept = default;
    Mock &operator=(const Mock &) = delete;
    Mock &operator=(Mock &&) noexcept = default;

    [[nodiscard]] auto handle() const {
        if constexpr (detail::FacadeContract<Contract>) {
            return pro::proxy<Contract>(detail::FacadePointer<Contract>(states));
        } else {
            Port result{};
            template for (constexpr auto method : methods()) {
                constexpr auto port_member = port_members()[index_of<method>()];
                auto state = state_for<method>();
                result.[:port_member:] = [state](auto &&...args) noexcept(std::meta::is_noexcept(method)) -> decltype(auto) {
                    return state->invoke(decltype(args)(args)...);
                };
            }
            return result;
        }
    }

    template <std::meta::info Operation>
    [[nodiscard]] auto expect() {
        if constexpr (detail::FacadeContract<Contract>) {
            constexpr auto convention = detail::resolve_convention<Contract, Operation>();
            using Convention = [:convention:];
            auto state = facade_state_for<Convention>();
            state->expect();
            return Expectation<detail::convention_signature_t<Convention>>(std::move(state));
        } else {
            constexpr auto method = detail::resolve_method<Contract, Operation>();
            auto state = state_for<method>();
            state->expect();
            return Expectation<detail::method_signature_t<method>>(std::move(state));
        }
    }

    template <typename Dispatch>
        requires detail::FacadeContract<Contract>
    [[nodiscard]] auto expect() {
        return expect<^^Dispatch>();
    }

    template <std::meta::info Operation>
    [[nodiscard]] auto allow() {
        if constexpr (detail::FacadeContract<Contract>) {
            constexpr auto convention = detail::resolve_convention<Contract, Operation>();
            using Convention = [:convention:];
            auto state = facade_state_for<Convention>();
            state->allow();
            return Expectation<detail::convention_signature_t<Convention>>(std::move(state));
        } else {
            constexpr auto method = detail::resolve_method<Contract, Operation>();
            auto state = state_for<method>();
            state->allow();
            return Expectation<detail::method_signature_t<method>>(std::move(state));
        }
    }

    template <typename Dispatch>
        requires detail::FacadeContract<Contract>
    [[nodiscard]] auto allow() {
        return allow<^^Dispatch>();
    }

    void verify() const {
        std::vector<std::string> failures;
        if constexpr (detail::FacadeContract<Contract>) {
            template for (constexpr auto convention : conventions()) {
                using Convention = [:convention:];
                if (auto failure = facade_state_for<Convention>()->verification_error()) {
                    failures.push_back(std::move(*failure));
                }
            }
        } else {
            template for (constexpr auto method : methods()) {
                if (auto failure = state_for<method>()->verification_error()) {
                    failures.push_back(std::move(*failure));
                }
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

    static consteval auto conventions()
        requires detail::FacadeContract<Contract>
    {
        return std::define_static_array(detail::facade_conventions<Contract>());
    }

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

    template <typename Convention>
        requires detail::FacadeContract<Contract>
    [[nodiscard]] std::shared_ptr<detail::convention_state_t<Convention>> facade_state_for() const {
        return std::static_pointer_cast<detail::convention_state_t<Convention>>(states[detail::convention_index<Contract, Convention>()]);
    }

    static consteval usize operation_count() {
        if constexpr (detail::FacadeContract<Contract>) {
            return std::tuple_size_v<typename Contract::convention_types>;
        } else {
            return methods().size();
        }
    }

    std::array<std::shared_ptr<void>, operation_count()> states;
};

} // namespace lighter::mock

template <typename Contract>
struct pro::is_bitwise_trivially_relocatable<lighter::mock::detail::FacadePointer<Contract>>
    : std::bool_constant<pro::is_bitwise_trivially_relocatable_v<
          lighter::mock::detail::FacadeAccessors<lighter::mock::detail::FacadePointer<Contract>, Contract, true>
      >> {};
