#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct DocumentMarkupDialect : u8 {
    GRAPHVIZ,
    BLOCKDIAG,
    LATEX,
    RESTRUCTURED_TEXT,
    TEXINFO,
    TYPST,
};

struct DocumentMarkupLexer {
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
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    DocumentMarkupDialect dialect = DocumentMarkupDialect::LATEX;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case DocumentMarkupDialect::GRAPHVIZ: return {.id = "graphviz", .name = "GraphViz"};
            case DocumentMarkupDialect::BLOCKDIAG: return {.id = "blockdiag", .name = "blockdiag"};
            case DocumentMarkupDialect::LATEX: return {.id = "latex", .name = "LaTeX"};
            case DocumentMarkupDialect::RESTRUCTURED_TEXT: return {.id = "rst", .name = "reStructuredText"};
            case DocumentMarkupDialect::TEXINFO: return {.id = "texinfo", .name = "Texinfo"};
            case DocumentMarkupDialect::TYPST: return {.id = "typst", .name = "Typst"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
