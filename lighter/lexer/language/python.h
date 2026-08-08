#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

struct PythonLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        CLASS[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        BUILTIN[[= token_role(TokenRole::FUNCTION)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        RAW_STRING[[= token_role(TokenRole::STRING)]],
        FORMAT_STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DECORATOR[[= token_role(TokenRole::ATTRIBUTE)]],
    };

    static constexpr LanguageInfo language{.id = "python", .name = "Python"};

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
