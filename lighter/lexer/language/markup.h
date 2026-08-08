#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct MarkupDialect : u8 {
    HTML,
    XML,
    MARKDOWN,
};

struct MarkupLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOCUMENTATION[[= token_role(TokenRole::DOCUMENTATION)]],
        TAG[[= token_role(TokenRole::TYPE)]],
        ATTRIBUTE[[= token_role(TokenRole::PROPERTY)]],
        VALUE[[= token_role(TokenRole::STRING)]],
        ENTITY[[= token_role(TokenRole::ESCAPE)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        HEADING[[= token_role(TokenRole::MODULE)]],
        LINK[[= token_role(TokenRole::MODULE)]],
        CODE[[= token_role(TokenRole::STRING)]],
        FENCE[[= token_role(TokenRole::LABEL)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    MarkupDialect dialect = MarkupDialect::HTML;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case MarkupDialect::HTML: return {.id = "html", .name = "HTML"};
            case MarkupDialect::XML: return {.id = "xml", .name = "XML"};
            case MarkupDialect::MARKDOWN: return {.id = "markdown", .name = "Markdown"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
