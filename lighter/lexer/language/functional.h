#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct FunctionalDialect : u8 {
    LISP,
    HASKELL,
    OCAML,
    FSHARP,
    ERLANG,
    ELIXIR,
};

struct FunctionalLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOCUMENTATION[[= token_role(TokenRole::DOCUMENTATION)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        PARAMETER[[= token_role(TokenRole::PARAMETER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        CHARACTER[[= token_role(TokenRole::CHARACTER)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    FunctionalDialect dialect = FunctionalDialect::LISP;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case FunctionalDialect::LISP: return {.id = "lisp", .name = "Lisp"};
            case FunctionalDialect::HASKELL: return {.id = "haskell", .name = "Haskell"};
            case FunctionalDialect::OCAML: return {.id = "ocaml", .name = "OCaml"};
            case FunctionalDialect::FSHARP: return {.id = "fsharp", .name = "F#"};
            case FunctionalDialect::ERLANG: return {.id = "erlang", .name = "Erlang"};
            case FunctionalDialect::ELIXIR: return {.id = "elixir", .name = "Elixir"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
