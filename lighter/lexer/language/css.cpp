#include "css.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexCSS.cxx and stlCSS.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native port retains CSS,
// SCSS, and Less selectors, at-rules, variables, declaration properties,
// functions, dimensions, strings, escapes, and multiline comments while
// removing folding and Scintilla document properties.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = CssLexer::Style;

constexpr auto k_values =
    make_word_set("absolute", "auto", "block", "bold", "border-box", "both", "bottom", "center", "collapse", "column", "contain",
                  "content-box", "currentColor", "dashed", "default", "fixed", "flex", "grid", "hidden", "inherit", "initial", "inline",
                  "inline-block", "left", "none", "normal", "nowrap", "relative", "repeat", "right", "row", "solid", "space-around",
                  "space-between", "static", "sticky", "top", "transparent", "unset", "visible", "wrap");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_comment = 0x1000'0000;
constexpr u32 k_double_string = 0x2000'0000;
constexpr u32 k_single_string = 0x3000'0000;
constexpr u32 k_brace_depth_mask = 0x0000'ffff;

[[nodiscard]] usize content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize skip_space(std::string_view source, usize position, usize end) noexcept {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] bool name_start(char value) noexcept { return ascii_identifier_start(value) || value == '-' || value == '_'; }

[[nodiscard]] bool name_continue(char value) noexcept { return ascii_identifier_continue(value) || value == '-' || value == '_'; }

[[nodiscard]] usize escape_end(std::string_view source, usize position, usize end) noexcept {
    ++position;
    usize digits = 0;
    while (position < end && digits < 6 && ascii_hex_digit(source[position])) {
        ++position;
        ++digits;
    }
    if (digits == 0 && position < end) ++position;
    if (position < end && ascii_space(source[position])) ++position;
    return position;
}

void paint_escapes(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] != '\\') {
            ++position;
            continue;
        }
        const usize token_end = escape_end(context.source, position, end);
        paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
        position = token_end;
    }
}

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote) noexcept {
    while (position < end) {
        if (source[position] == '\\')
            position = escape_end(source, position, end);
        else if (source[position] == quote)
            return position + 1;
        else
            ++position;
    }
    return position;
}

[[nodiscard]] bool number_continue(char value) noexcept {
    return ascii_alphanumeric(value) || value == '.' || value == '%' || value == '-';
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    usize brace_depth = state & k_brace_depth_mask;
    bool declaration_name = brace_depth != 0;
    while (position < line_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_comment) {
            const usize found = context.source.find("*/", position);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) break;
            state = static_cast<u32>(brace_depth);
            continue;
        }
        if (kind == k_double_string || kind == k_single_string) {
            const char quote = kind == k_double_string ? '"' : '\'';
            const usize token_end = quoted_end(context.source, position, line_end, quote);
            paint(context, {.begin = position, .end = token_end}, Style::STRING);
            paint_escapes(context, position, token_end);
            position = token_end;
            if (token_end == line_end || context.source[token_end - 1] != quote) break;
            state = static_cast<u32>(brace_depth);
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (context.source.substr(position).starts_with("/*")) {
            state = k_comment | static_cast<u32>(brace_depth);
        } else if (context.source.substr(position).starts_with("//")) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_escapes(context, token_begin, position);
            if (position == line_end && context.source[position - 1] != current)
                state = (current == '"' ? k_double_string : k_single_string) | static_cast<u32>(brace_depth);
        } else if (current == '@') {
            const usize token_begin = position++;
            while (position < line_end && name_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, brace_depth == 0 ? Style::AT_RULE : Style::VARIABLE);
        } else if (current == '$' && position + 1 < line_end && name_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && name_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::VARIABLE);
        } else if ((current == '.' || current == '#') && position + 1 < line_end && name_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && name_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, current == '.' ? Style::CLASS : Style::ID);
        } else if (current == ':' && position + 1 < line_end && name_start(context.source[position + 1])) {
            const usize token_begin = position++;
            if (position < line_end && context.source[position] == ':') ++position;
            while (position < line_end && name_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::PSEUDO);
        } else if (ascii_digit(current) || (current == '.' && position + 1 < line_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < line_end && number_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (name_start(current)) {
            const usize token_begin = position++;
            while (position < line_end && name_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const usize next = skip_space(context.source, position, line_end);
            Style style = Style::SELECTOR;
            if (brace_depth != 0 || declaration_name) {
                if (next < line_end && context.source[next] == ':')
                    style = Style::PROPERTY;
                else if (next < line_end && context.source[next] == '(')
                    style = Style::FUNCTION;
                else if (k_values.contains(word))
                    style = Style::VALUE;
                else
                    style = Style::DEFAULT;
            }
            paint(context, {.begin = token_begin, .end = position}, style);
        } else if (current == '{') {
            ++brace_depth;
            declaration_name = true;
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else if (current == '}') {
            if (brace_depth != 0) --brace_depth;
            declaration_name = brace_depth != 0;
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else if (std::string_view("[]():;,>+~*=|&!").contains(current)) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else if (current == '\\') {
            const usize token_end = escape_end(context.source, position, line_end);
            paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
            position = token_end;
        } else {
            ++position;
        }
    }
    if (end > line_end && (state & k_state_mask) != k_normal)
        paint(context, {.begin = line_end, .end = end}, (state & k_state_mask) == k_comment ? Style::COMMENT : Style::STRING);
    return (state & k_state_mask) | static_cast<u32>(std::min<usize>(brace_depth, k_brace_depth_mask));
}

} // namespace

void CssLexer::lex(LexContext &context) const {
    const auto first_next = std::ranges::upper_bound(context.line_starts, context.range.begin);
    usize line = static_cast<usize>(first_next - context.line_starts.begin() - 1);
    contract_assert(context.line_starts[line] == context.range.begin);
    while (line < context.line_starts.size()) {
        const usize begin = context.line_starts[line];
        const usize end =
            std::min(context.range.end, line + 1 < context.line_starts.size() ? context.line_starts[line + 1] : context.source.size());
        const u32 next_state = lex_line(context, begin, end, context.line_states[line]);
        if (line + 1 < context.line_states.size() && end == context.line_starts[line + 1]) context.line_states[line + 1] = next_state;
        if (end >= context.range.end) break;
        ++line;
    }
}

} // namespace lighter::lexer
