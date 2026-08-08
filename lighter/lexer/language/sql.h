#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

struct SqlLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        QUOTED_IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        PARAMETER[[= token_role(TokenRole::PARAMETER)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
    };

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept { return {.id = "sql", .name = "SQL"}; }
    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
