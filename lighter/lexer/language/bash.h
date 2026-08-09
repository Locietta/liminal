#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct ShellDialect : u8 {
    BASH,
    C_SHELL,
    M4,
};

struct BashLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        VARIABLE[[= token_role(TokenRole::PROPERTY)]],
        PARAMETER[[= token_role(TokenRole::PARAMETER)]],
        OPTION[[= token_role(TokenRole::ATTRIBUTE)]],
        STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        HEREDOC_DELIMITER[[= token_role(TokenRole::LABEL)]],
        HEREDOC[[= token_role(TokenRole::STRING)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    ShellDialect dialect = ShellDialect::BASH;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case ShellDialect::BASH: return {.id = "bash", .name = "Shell Script"};
            case ShellDialect::C_SHELL: return {.id = "csh", .name = "C Shell Script"};
            case ShellDialect::M4: return {.id = "m4", .name = "M4 Script"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
