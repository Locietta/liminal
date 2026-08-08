#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct LegacyDialect : u8 {
    APDL,
    ABAQUS,
    FORTRAN,
    PASCAL,
    POWERBUILDER,
    SAS,
    VISUAL_BASIC,
    VBSCRIPT,
};

struct LegacyLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOCUMENTATION[[= token_role(TokenRole::DOCUMENTATION)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        VARIABLE[[= token_role(TokenRole::PROPERTY)]],
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

    LegacyDialect dialect = LegacyDialect::FORTRAN;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case LegacyDialect::APDL: return {.id = "apdl", .name = "ANSYS APDL"};
            case LegacyDialect::ABAQUS: return {.id = "abaqus", .name = "ABAQUS"};
            case LegacyDialect::FORTRAN: return {.id = "fortran", .name = "Fortran"};
            case LegacyDialect::PASCAL: return {.id = "pascal", .name = "Pascal"};
            case LegacyDialect::POWERBUILDER: return {.id = "powerbuilder", .name = "PowerBuilder"};
            case LegacyDialect::SAS: return {.id = "sas", .name = "SAS"};
            case LegacyDialect::VISUAL_BASIC: return {.id = "vb", .name = "Visual Basic"};
            case LegacyDialect::VBSCRIPT: return {.id = "vbscript", .name = "VBScript"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
