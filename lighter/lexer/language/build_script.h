#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct BuildScriptDialect : u8 {
    BATCH,
    CMAKE,
    GN,
    MAKE,
    JAM,
    INNO_SETUP,
    NSIS,
};

struct BuildScriptLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        VARIABLE[[= token_role(TokenRole::PROPERTY)]],
        PARAMETER[[= token_role(TokenRole::PARAMETER)]],
        STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        SECTION[[= token_role(TokenRole::MODULE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    BuildScriptDialect dialect = BuildScriptDialect::CMAKE;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case BuildScriptDialect::BATCH: return {.id = "batch", .name = "Windows Batch"};
            case BuildScriptDialect::CMAKE: return {.id = "cmake", .name = "CMake"};
            case BuildScriptDialect::GN: return {.id = "gn", .name = "GN"};
            case BuildScriptDialect::MAKE: return {.id = "make", .name = "Makefile"};
            case BuildScriptDialect::JAM: return {.id = "jam", .name = "Jamfile"};
            case BuildScriptDialect::INNO_SETUP: return {.id = "inno", .name = "Inno Setup"};
            case BuildScriptDialect::NSIS: return {.id = "nsis", .name = "NSIS"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
