#pragma once

#include <memory>
#include <meta>
#include <optional>
#include <type_traits>

#include <lighter/types.hpp>

/// Trivially-relocatable trait, P2786-flavoured, built on C++26 reflection
/// until the language trait ships in our toolchain.
///
/// A type is trivially relocatable when move-construct + destroy-source is
/// equivalent to memcpy. Containers use this to relocate buffers bytewise;
/// after a bytewise relocation the source lifetimes end WITHOUT running
/// destructors (see mem::uninitialized_relocate for the caller contract).
///
/// Detection order for class types:
///   1. explicit annotation  [[=trivially_relocatable]] / (false) - always
///      wins, including an opt-out on a trivially copyable type (which forces
///      containers onto the move+destroy path and poisons enclosing types)
///   2. trivially copyable types
///   3. rule-of-zero heuristic: movable, non-polymorphic, no user-provided
///      copy/move/destroy machinery, no virtual bases, and all bases +
///      members recursively trivially relocatable
///
/// The recursion re-enters the public is_trivially_relocatable_v template,
/// so user specializations (like the std ones below) apply at any depth.
///
/// The heuristic is deliberately conservative: a defaulted move constructor
/// memberwise-moves, so relocatability composes; any user-provided special
/// member could observe the object's address, so it disables the fast path.
///
/// Annotation contract: a type annotated [[=trivially_relocatable]] must not
/// have potentially-throwing copy/move constructors observable by containers;
/// heuristic-derived types satisfy this by construction (all special members
/// defaulted recursively).
namespace lighter::mem {

/// Explicit opt-in/out:
///   struct [[=trivially_relocatable]] SelfContained { ... };
///   struct [[=trivially_relocatable(false)]] AddressSensitive { ... };
struct TriviallyRelocatable {
    bool value = true;

    constexpr TriviallyRelocatable operator()(bool v) const noexcept { return {.value = v}; }
};

inline constexpr TriviallyRelocatable trivially_relocatable{};

namespace detail::reloc {

consteval bool entry(std::meta::info type);

} // namespace detail::reloc

template <typename T>
constexpr bool is_trivially_relocatable_v = detail::reloc::entry(^^T);

/// Standard types that are relocatable in every practical implementation but
/// fail the rule-of-zero heuristic (user-provided moves). Extend as needed.
template <typename T, typename D>
constexpr bool is_trivially_relocatable_v<std::unique_ptr<T, D>> = std::is_empty_v<D> || is_trivially_relocatable_v<D>;

template <typename T>
constexpr bool is_trivially_relocatable_v<std::shared_ptr<T>> = true;

template <typename T>
constexpr bool is_trivially_relocatable_v<std::weak_ptr<T>> = true;

namespace detail::reloc {

consteval std::optional<bool> annotation_override(std::meta::info type) {
    for (auto annotation : std::meta::annotations_of(type)) {
        if (std::meta::type_of(annotation) == ^^const TriviallyRelocatable) {
            return std::meta::extract<TriviallyRelocatable>(annotation).value;
        }
    }
    return std::nullopt;
}

/// Re-enter the public variable template, so user specializations
/// participate at any recursion depth.
consteval bool via_trait(std::meta::info type) {
    return std::meta::extract<bool>(std::meta::substitute(^^is_trivially_relocatable_v, {type}));
}

consteval bool has_user_provided_special_member(std::meta::info type) {
    for (auto member : std::meta::members_of(type, std::meta::access_context::unchecked())) {
        if (!std::meta::is_special_member_function(member)) {
            continue;
        }
        const bool relocation_relevant = std::meta::is_destructor(member) || std::meta::is_move_constructor(member) ||
                                         std::meta::is_copy_constructor(member) || std::meta::is_move_assignment(member) ||
                                         std::meta::is_copy_assignment(member);
        if (relocation_relevant && std::meta::is_user_provided(member)) {
            return true;
        }
    }
    return false;
}

consteval bool class_is_trivially_relocatable(std::meta::info type) {
    if (auto forced = annotation_override(type)) {
        return *forced;
    }

    if (!std::meta::is_trivially_copyable_type(type)) {
        if (!std::meta::is_move_constructible_type(type) || std::meta::is_polymorphic_type(type)) {
            return false;
        }
        if (has_user_provided_special_member(type)) {
            return false;
        }
    }

    for (auto base : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
        if (std::meta::is_virtual(base) || !via_trait(std::meta::type_of(base))) {
            return false;
        }
    }

    for (auto member : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked())) {
        auto member_type = std::meta::type_of(member);
        // reference members are pointer-like for relocation purposes
        if (!std::meta::is_reference_type(member_type) && !via_trait(member_type)) {
            return false;
        }
    }

    return true;
}

consteval bool entry(std::meta::info type) {
    auto stripped = std::meta::remove_cv(std::meta::remove_all_extents(type));
    if (stripped != type) {
        // re-enter through the variable template so cv/array-of-T pick up
        // specializations for T
        return via_trait(stripped);
    }

    if (std::meta::is_scalar_type(type)) {
        return true;
    }
    if (std::meta::is_class_type(type)) {
        return class_is_trivially_relocatable(type);
    }
    return false; // references, functions, void
}

} // namespace detail::reloc

} // namespace lighter::mem
