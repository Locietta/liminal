#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

struct CssLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        AT_RULE[[= token_role(TokenRole::PREPROCESSOR)]],
        SELECTOR[[= token_role(TokenRole::TYPE)]],
        CLASS[[= token_role(TokenRole::ATTRIBUTE)]],
        ID[[= token_role(TokenRole::CONSTANT)]],
        PSEUDO[[= token_role(TokenRole::FUNCTION)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        VARIABLE[[= token_role(TokenRole::IDENTIFIER)]],
        VALUE[[= token_role(TokenRole::KEYWORD)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept { return {.id = "css", .name = "CSS"}; }
    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
