#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct StructuredDataDialect : u8 {
    JSON,
    TOML,
    YAML,
    CONFIG,
    INI,
    CSV,
    TSV,
    DIFF,
};

struct StructuredDataLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        KEY[[= token_role(TokenRole::PROPERTY)]],
        SECTION[[= token_role(TokenRole::MODULE)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        ANCHOR[[= token_role(TokenRole::LABEL)]],
        TAG[[= token_role(TokenRole::TYPE)]],
        STRING[[= token_role(TokenRole::STRING)]],
        BLOCK_STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        ADDED[[= token_role(TokenRole::STRING)]],
        REMOVED[[= token_role(TokenRole::COMMENT)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    StructuredDataDialect dialect = StructuredDataDialect::JSON;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case StructuredDataDialect::JSON: return {.id = "json", .name = "JSON"};
            case StructuredDataDialect::TOML: return {.id = "toml", .name = "TOML"};
            case StructuredDataDialect::YAML: return {.id = "yaml", .name = "YAML"};
            case StructuredDataDialect::CONFIG: return {.id = "config", .name = "Configuration"};
            case StructuredDataDialect::INI: return {.id = "ini", .name = "INI"};
            case StructuredDataDialect::CSV: return {.id = "csv", .name = "CSV"};
            case StructuredDataDialect::TSV: return {.id = "tsv", .name = "TSV"};
            case StructuredDataDialect::DIFF: return {.id = "diff", .name = "Diff"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
