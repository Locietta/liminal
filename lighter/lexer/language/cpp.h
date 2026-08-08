#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct CppDialect : u8 {
    C,
    CPP,
    OBJECTIVE_C,
    OBJECTIVE_CPP,
    RESOURCE_SCRIPT,
    IDL,
};

struct CppLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOC_COMMENT[[= token_role(TokenRole::DOCUMENTATION)]],
        DOC_TAG[[= token_role(TokenRole::ATTRIBUTE)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        CLASS[[= token_role(TokenRole::TYPE)]],
        STRUCT[[= token_role(TokenRole::TYPE)]],
        UNION[[= token_role(TokenRole::TYPE)]],
        ENUMERATION[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        RAW_STRING[[= token_role(TokenRole::STRING)]],
        CHARACTER[[= token_role(TokenRole::CHARACTER)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        PREPROCESSOR[[= token_role(TokenRole::PREPROCESSOR)]],
        HEADER[[= token_role(TokenRole::MODULE)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
    };

    CppDialect dialect = CppDialect::CPP;

    [[nodiscard]] LanguageInfo language_info() const noexcept;
    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
