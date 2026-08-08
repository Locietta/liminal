#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

struct RustLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOC_COMMENT[[= token_role(TokenRole::DOCUMENTATION)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        STRUCT[[= token_role(TokenRole::TYPE)]],
        TRAIT[[= token_role(TokenRole::TYPE)]],
        ENUMERATION[[= token_role(TokenRole::TYPE)]],
        UNION[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        BYTE_STRING[[= token_role(TokenRole::STRING)]],
        CHARACTER[[= token_role(TokenRole::CHARACTER)]],
        LIFETIME[[= token_role(TokenRole::LABEL)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        MACRO[[= token_role(TokenRole::FUNCTION)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
    };

    static constexpr LanguageInfo language{.id = "rust", .name = "Rust"};

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
