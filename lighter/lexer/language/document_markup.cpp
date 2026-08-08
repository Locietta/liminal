#include "document_markup.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexGraphViz.cxx, LexLaTeX.cxx, LexReST.cxx,
// LexTexinfo.cxx, LexTypst.cxx, and matching stl*.cpp data at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. GraphViz and blockdiag remain
// distinct identities over their shared upstream lexer. The native port keeps
// directives, commands, roles, attributes, headings, labels, math/string
// spans, verbatim regions, ignored regions, and streamed block comments.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = DocumentMarkupDialect;
using Style = DocumentMarkupLexer::Style;

constexpr auto k_graph_keywords = make_word_set("digraph", "edge", "graph", "node", "strict", "subgraph");
constexpr auto k_blockdiag_keywords = make_word_set("blockdiag", "diagram", "edge", "group", "node", "orientation", "shape", "style");
constexpr auto k_rst_directives =
    make_word_set("admonition", "attention", "caution", "code", "code-block", "contents", "csv-table", "danger", "date", "error", "figure",
                  "footer", "header", "hint", "image", "important", "include", "list-table", "math", "note", "raw", "replace", "role",
                  "rubric", "table", "tip", "title", "topic", "unicode", "warning");
constexpr auto k_texinfo_commands = make_word_set(
    "anchor", "appendix", "author", "bye", "chapter", "cindex", "code", "command", "contents", "copying", "defcodeindex", "defcv", "deffn",
    "defivar", "defmac", "defmethod", "defop", "defopt", "defspec", "deftp", "defun", "defvar", "display", "documentdescription", "end",
    "enumerate", "example", "file", "include", "item", "itemize", "menu", "node", "paragraphindent", "printindex", "section", "set",
    "setfilename", "subsection", "title", "top", "uref", "value", "verbatim", "xref");
constexpr auto k_typst_keywords = make_word_set("and", "as", "break", "context", "continue", "else", "false", "for", "if", "import", "in",
                                                "include", "let", "none", "not", "or", "return", "set", "show", "true", "while");
constexpr auto k_constants = make_word_set("false", "none", "true");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_verbatim = 0x2000'0000;
constexpr u32 k_ignore = 0x3000'0000;

[[nodiscard]] usize content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize skip_space(std::string_view source, usize position, usize end) noexcept {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] usize previous_non_space(std::string_view source, usize position, usize begin) noexcept {
    while (position > begin && ascii_space(source[position - 1])) --position;
    return position;
}

[[nodiscard]] bool graph_dialect(Dialect dialect) noexcept { return dialect == Dialect::GRAPHVIZ || dialect == Dialect::BLOCKDIAG; }

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '-' || value == ':';
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\#$").contains(value); }

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote) noexcept {
    while (position < end) {
        if (source[position] == '\\' && position + 1 < end)
            position += 2;
        else if (source[position++] == quote)
            return position;
    }
    return end;
}

void paint_escapes(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] usize command_end(std::string_view source, usize position, usize end) noexcept {
    if (position < end && !ascii_alpha(source[position])) return position + 1;
    while (position < end && (ascii_alpha(source[position]) || source[position] == '@')) ++position;
    return position;
}

[[nodiscard]] bool decoration_line(std::string_view source, usize first, usize line_end) noexcept {
    if (first >= line_end || !std::string_view("=-~^\"'`:+*#_").contains(source[first])) return false;
    const char value = source[first];
    for (usize position = first + 1; position < line_end; ++position) {
        if (source[position] != value) return false;
    }
    return line_end - first >= 3;
}

[[nodiscard]] u32 continue_state(LexContext &context, usize &position, usize line_end, u32 state, Dialect dialect) {
    const u32 kind = state & k_state_mask;
    if (kind == k_verbatim) {
        const usize close = context.source.find("\\end{verbatim}", position);
        const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + 14;
        paint(context, {.begin = position, .end = token_end}, Style::STRING);
        position = token_end;
        return close == std::string_view::npos || close >= line_end ? state : k_normal;
    }
    if (kind == k_ignore) {
        const usize first = skip_space(context.source, position, line_end);
        paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
        position = line_end;
        return context.source.substr(first).starts_with("@end ignore") ? k_normal : state;
    }
    u32 depth = std::max<u32>(1, state & k_payload_mask);
    const usize token_begin = position;
    while (position < line_end && depth != 0) {
        if (dialect == Dialect::TYPST && context.source.substr(position, 2) == "/*") {
            ++depth;
            position += 2;
        } else if (context.source.substr(position, 2) == "*/") {
            --depth;
            position += 2;
        } else {
            ++position;
        }
    }
    paint(context, {.begin = token_begin, .end = position}, Style::COMMENT);
    return depth == 0 ? k_normal : k_block_comment | depth;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    if ((state & k_state_mask) != k_normal) {
        state = continue_state(context, position, line_end, state, dialect);
        if (state != k_normal) {
            if (end > line_end)
                paint(context, {.begin = line_end, .end = end}, (state & k_state_mask) == k_verbatim ? Style::STRING : Style::COMMENT);
            return state;
        }
    }

    const usize first = skip_space(context.source, position, line_end);
    if (first >= line_end) return k_normal;
    if (dialect == Dialect::RESTRUCTURED_TEXT && decoration_line(context.source, first, line_end)) {
        paint(context, {.begin = first, .end = line_end}, Style::MODULE);
        return k_normal;
    }
    if (dialect == Dialect::RESTRUCTURED_TEXT && context.source.substr(first, 2) == "..") {
        const usize name_begin = skip_space(context.source, first + 2, line_end);
        usize name_end = name_begin;
        while (name_end < line_end && (ascii_identifier_continue(context.source[name_end]) || context.source[name_end] == '-')) ++name_end;
        if (context.source.substr(name_end, 2) == "::" &&
            k_rst_directives.contains(context.source.substr(name_begin, name_end - name_begin))) {
            paint(context, {.begin = first, .end = name_end + 2}, Style::DIRECTIVE);
            position = name_end + 2;
        } else {
            paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
            return k_normal;
        }
    }
    if (dialect == Dialect::TEXINFO && context.source.substr(first).starts_with("@ignore")) {
        paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
        return k_ignore;
    }
    if (dialect == Dialect::TYPST && context.source[first] == '=') {
        usize marker_end = first;
        while (marker_end < line_end && context.source[marker_end] == '=') ++marker_end;
        paint(context, {.begin = first, .end = marker_end}, Style::MODULE);
        position = marker_end;
    }

    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        const bool graph_comment = graph_dialect(dialect) && (context.source.substr(position, 2) == "//" || current == '#');
        if (graph_comment || (dialect == Dialect::LATEX && current == '%') ||
            (dialect == Dialect::TYPST && context.source.substr(position, 2) == "//")) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if ((graph_dialect(dialect) || dialect == Dialect::TYPST) && context.source.substr(position, 2) == "/*") {
            const usize token_begin = position;
            position += 2;
            state = continue_state(context, position, line_end, k_block_comment | 1, dialect);
            paint(context, {.begin = token_begin, .end = token_begin + 2}, Style::COMMENT);
            if (state != k_normal) return state;
            continue;
        }
        if (dialect == Dialect::LATEX && context.source.substr(position).starts_with("\\begin{verbatim}")) {
            paint(context, {.begin = position, .end = line_end}, Style::STRING);
            return k_verbatim;
        }
        if (dialect == Dialect::LATEX && current == '\\') {
            const usize token_begin = position++;
            position = command_end(context.source, position, line_end);
            const std::string_view command = context.source.substr(token_begin + 1, position - token_begin - 1);
            const bool directive = command == "documentclass" || command == "include" || command == "input" || command == "label" ||
                                   command == "package" || command == "usepackage";
            paint(context, {.begin = token_begin, .end = position}, directive ? Style::DIRECTIVE : Style::FUNCTION);
            continue;
        }
        if (dialect == Dialect::TEXINFO && current == '@') {
            if (context.source.substr(position).starts_with("@c ") || context.source.substr(position).starts_with("@comment")) {
                paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
                break;
            }
            const usize token_begin = position++;
            position = command_end(context.source, position, line_end);
            const std::string_view command = context.source.substr(token_begin + 1, position - token_begin - 1);
            Style style = k_texinfo_commands.contains(command) ? Style::DIRECTIVE : Style::FUNCTION;
            if (command == "node") style = Style::MODULE;
            paint(context, {.begin = token_begin, .end = position}, style);
            continue;
        }
        if (dialect == Dialect::RESTRUCTURED_TEXT && current == ':' && position + 1 < line_end) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            if (position < line_end && context.source[position] == ':') ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::ATTRIBUTE);
            continue;
        }
        if (dialect == Dialect::TYPST && current == '#' && position + 1 < line_end && identifier_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin + 1, position - token_begin - 1);
            if (k_typst_keywords.contains(word)) {
                paint(context, {.begin = token_begin, .end = token_begin + 1}, Style::DIRECTIVE);
                position = token_begin + 1;
                continue;
            }
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  next < line_end && context.source[next] == '(' ? Style::FUNCTION : Style::DIRECTIVE);
            continue;
        }
        if (current == '"' || current == '\'' || current == '`') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_escapes(context, token_begin, position);
            continue;
        }
        if ((dialect == Dialect::LATEX || dialect == Dialect::TYPST) && current == '$') {
            const usize token_begin = position++;
            const usize close = context.source.find('$', position);
            position = close == std::string_view::npos || close >= line_end ? line_end : close + 1;
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            continue;
        }
        if (graph_dialect(dialect) && current == '<') {
            const usize token_begin = position++;
            usize depth = 1;
            while (position < line_end && depth != 0) {
                if (context.source[position] == '<')
                    ++depth;
                else if (context.source[position] == '>')
                    --depth;
                ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            continue;
        }
        if (ascii_digit(current)) {
            const usize token_begin = position++;
            while (position < line_end &&
                   (ascii_alphanumeric(context.source[position]) || std::string_view("_.'+-").contains(context.source[position])))
                ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            continue;
        }
        if (identifier_start(current)) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const usize next = skip_space(context.source, position, line_end);
            const usize previous = previous_non_space(context.source, token_begin, begin);
            Style style = Style::IDENTIFIER;
            if ((dialect == Dialect::GRAPHVIZ && k_graph_keywords.contains(word)) ||
                (dialect == Dialect::BLOCKDIAG && (k_graph_keywords.contains(word) || k_blockdiag_keywords.contains(word))) ||
                (dialect == Dialect::TYPST && k_typst_keywords.contains(word))) {
                style = k_constants.contains(word) ? Style::CONSTANT : Style::KEYWORD;
            } else if (graph_dialect(dialect) && next < line_end && context.source[next] == '=') {
                style = Style::PROPERTY;
            } else if (next < line_end && context.source[next] == '(') {
                style = Style::FUNCTION;
            } else if (previous > begin && context.source[previous - 1] == '.') {
                style = Style::PROPERTY;
            }
            paint(context, {.begin = token_begin, .end = position}, style);
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < line_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            continue;
        }
        ++position;
    }
    return k_normal;
}

} // namespace

void DocumentMarkupLexer::lex(LexContext &context) const {
    const auto first_next = std::ranges::upper_bound(context.line_starts, context.range.begin);
    usize line = static_cast<usize>(first_next - context.line_starts.begin() - 1);
    contract_assert(context.line_starts[line] == context.range.begin);
    while (line < context.line_starts.size()) {
        const usize begin = context.line_starts[line];
        const usize end =
            std::min(context.range.end, line + 1 < context.line_starts.size() ? context.line_starts[line + 1] : context.source.size());
        const u32 next_state = lex_line(context, begin, end, context.line_states[line], dialect);
        if (line + 1 < context.line_states.size() && end == context.line_starts[line + 1]) context.line_states[line + 1] = next_state;
        if (end >= context.range.end) break;
        ++line;
    }
}

} // namespace lighter::lexer
