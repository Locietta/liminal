#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct JavaScriptDialect : u8 {
    JAVASCRIPT,
    ACTIONSCRIPT,
    JSX,
    TYPESCRIPT,
    TSX,
};

struct JavaScriptLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOC_COMMENT[[= token_role(TokenRole::DOCUMENTATION)]],
        DOC_TAG[[= token_role(TokenRole::ATTRIBUTE)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        CLASS[[= token_role(TokenRole::TYPE)]],
        INTERFACE[[= token_role(TokenRole::TYPE)]],
        ENUMERATION[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        TEMPLATE[[= token_role(TokenRole::STRING)]],
        REGEX[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DECORATOR[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
    };

    JavaScriptDialect dialect = JavaScriptDialect::JAVASCRIPT;

    [[nodiscard]] LanguageInfo language_info() const noexcept;
    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
