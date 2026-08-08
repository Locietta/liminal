#pragma once

#include <concepts>
#include <string_view>
#include <utility>

#include <proxy/proxy.h>

#include <lighter/lexer/document.h>
#include <lighter/lexer/role.h>

namespace lighter::lexer {

struct LanguageInfo {
    std::string_view id;
    std::string_view name;
};

template <typename T>
concept LexerImplementation = requires(const T &implementation, LexContext &lex_context) {
    typename T::Style;
    requires std::same_as<std::underlying_type_t<typename T::Style>, u8>;
    { implementation.language_info() } -> std::convertible_to<LanguageInfo>;
    { implementation.lex(lex_context) } -> std::same_as<void>;
};

/// Supplies all reflected metadata behavior once, so concrete language lexers
/// contain only their data and lexing algorithm.
template <LexerImplementation Implementation>
struct ReflectedLexer {
    Implementation implementation;

    void lex(LexContext &lex_context) const { implementation.lex(lex_context); }

    [[nodiscard]] LanguageInfo language_info() const noexcept { return implementation.language_info(); }

    [[nodiscard]] TokenRole role_for_style(u8 style) const noexcept { return lexer::role_for_style<typename Implementation::Style>(style); }
};

PRO_DEF_MEM_DISPATCH(LexDispatch, lex);
PRO_DEF_MEM_DISPATCH(LanguageInfoDispatch, language_info);
PRO_DEF_MEM_DISPATCH(RoleForStyleDispatch, role_for_style);

struct LexerFacade : pro::facade_builder::add_convention<LexDispatch, void(LexContext &) const>::add_convention<
                         LanguageInfoDispatch, LanguageInfo() const noexcept
                     >::add_convention<RoleForStyleDispatch, TokenRole(u8) const noexcept>::build {};

using Lexer = pro::proxy<LexerFacade>;
using LexerView = pro::proxy_view<LexerFacade>;

template <LexerImplementation Implementation>
[[nodiscard]] ReflectedLexer<Implementation> reflect_lexer(Implementation implementation) {
    return {.implementation = std::move(implementation)};
}

template <LexerImplementation Implementation>
[[nodiscard]] Lexer make_lexer(Implementation implementation) {
    return pro::make_proxy<LexerFacade, ReflectedLexer<Implementation>>(std::move(implementation));
}

} // namespace lighter::lexer
