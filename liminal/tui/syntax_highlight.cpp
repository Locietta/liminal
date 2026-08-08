#include "syntax_highlight.h"

#include <string>

#include <lighter/lexer/registry.h>
#include <lighter/lexer/role.h>

namespace liminal::tui {

namespace {

using lighter::lexer::TokenRole;

constexpr usize k_max_highlight_bytes = 512 * 1024;
constexpr usize k_max_highlight_lines = 10'000;
constexpr usize k_max_highlight_line_bytes = 4 * 1024;

[[nodiscard]] Style style_for_role(TokenRole role) noexcept {
    switch (role) {
        case TokenRole::DEFAULT:
        case TokenRole::IDENTIFIER:
        case TokenRole::PARAMETER: return Style::NORMAL;
        case TokenRole::COMMENT:
        case TokenRole::DOCUMENTATION: return Style::CODE_COMMENT;
        case TokenRole::KEYWORD:
        case TokenRole::PREPROCESSOR: return Style::CODE_KEYWORD;
        case TokenRole::TYPE:
        case TokenRole::MODULE: return Style::CODE_TYPE;
        case TokenRole::FUNCTION: return Style::CODE_FUNCTION;
        case TokenRole::PROPERTY:
        case TokenRole::ATTRIBUTE:
        case TokenRole::LABEL: return Style::CODE_PROPERTY;
        case TokenRole::CONSTANT: return Style::CODE_CONSTANT;
        case TokenRole::STRING:
        case TokenRole::CHARACTER:
        case TokenRole::ESCAPE: return Style::CODE_STRING;
        case TokenRole::NUMBER: return Style::CODE_NUMBER;
        case TokenRole::OPERATOR: return Style::CODE_OPERATOR;
        case TokenRole::UNRECOGNIZED: return Style::CODE;
    }
    return Style::CODE;
}

void append_span(std::vector<StyledSpan> &spans, std::string_view text, Style style) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
        spans.back().text += text;
    } else {
        spans.push_back({.text = std::string(text), .style = style});
    }
}

[[nodiscard]] std::vector<StyledSpan> fallback(std::string_view line) {
    if (line.empty()) return {};
    return {{.text = std::string(line), .style = Style::CODE}};
}

} // namespace

CodeHighlighter::CodeHighlighter(std::string_view language) : lexer(lighter::lexer::lexer_for_language(language)) {}

std::vector<StyledSpan> CodeHighlighter::highlight_line(std::string_view line) {
    total_bytes += line.size();
    ++line_count;
    if (line.size() > k_max_highlight_line_bytes || total_bytes > k_max_highlight_bytes || line_count > k_max_highlight_lines) {
        enabled = false;
    }
    if (!supported()) return fallback(line);

    const usize line_begin = document.source.size();
    std::string appended(line);
    appended.push_back('\n');
    const lighter::lexer::LexRange dirty = lighter::lexer::append(document, appended);
    auto lex_context = lighter::lexer::context(document, dirty);
    (*lexer)->lex(lex_context);

    std::vector<StyledSpan> spans;
    const usize line_end = line_begin + line.size();
    for (usize begin = line_begin; begin < line_end;) {
        const u8 lexer_style = document.styles[begin];
        usize end = begin + 1;
        while (end < line_end && document.styles[end] == lexer_style) ++end;
        const TokenRole role = (*lexer)->role_for_style(lexer_style);
        append_span(spans, std::string_view(document.source).substr(begin, end - begin), style_for_role(role));
        begin = end;
    }
    return spans;
}

} // namespace liminal::tui
