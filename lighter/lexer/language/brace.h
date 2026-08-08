#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct BraceDialect : u8 {
    JAVA,
    CSHARP,
    D,
    DART,
    CANGJIE,
    GROOVY,
    GRADLE,
    HAXE,
    KOTLIN,
    SCALA,
    SWIFT,
    ZIG,
    ASYMPTOTE,
};

struct BraceLexer {
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
        PREPROCESSOR[[= token_role(TokenRole::PREPROCESSOR)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    BraceDialect dialect = BraceDialect::JAVA;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case BraceDialect::JAVA: return {.id = "java", .name = "Java"};
            case BraceDialect::CSHARP: return {.id = "csharp", .name = "C#"};
            case BraceDialect::D: return {.id = "d", .name = "D"};
            case BraceDialect::DART: return {.id = "dart", .name = "Dart"};
            case BraceDialect::CANGJIE: return {.id = "cangjie", .name = "Cangjie"};
            case BraceDialect::GROOVY: return {.id = "groovy", .name = "Groovy"};
            case BraceDialect::GRADLE: return {.id = "gradle", .name = "Gradle"};
            case BraceDialect::HAXE: return {.id = "haxe", .name = "Haxe"};
            case BraceDialect::KOTLIN: return {.id = "kotlin", .name = "Kotlin"};
            case BraceDialect::SCALA: return {.id = "scala", .name = "Scala"};
            case BraceDialect::SWIFT: return {.id = "swift", .name = "Swift"};
            case BraceDialect::ZIG: return {.id = "zig", .name = "Zig"};
            case BraceDialect::ASYMPTOTE: return {.id = "asymptote", .name = "Asymptote"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
