#include "go.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexGo.cxx and stlGo.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native port retains raw
// strings, declarations, builtin classification, and format specifiers while
// removing Scintilla infrastructure, folding, and generated keyword indices.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = GoLexer::Style;

constexpr auto k_keywords = make_word_set("break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "false",
                                          "for", "func", "go", "goto", "if", "import", "interface", "iota", "map", "nil", "package",
                                          "range", "return", "select", "struct", "switch", "true", "type", "var");
constexpr auto k_types = make_word_set("bool", "byte", "complex128", "complex64", "error", "float32", "float64", "int", "int16", "int32",
                                       "int64", "int8", "rune", "string", "uint", "uint16", "uint32", "uint64", "uint8", "uintptr");
constexpr auto k_builtins = make_word_set("append", "cap", "clear", "close", "complex", "copy", "delete", "imag", "len", "make", "max",
                                          "min", "new", "panic", "print", "println", "real", "recover");
constexpr auto k_constants = make_word_set("false", "iota", "nil", "true");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_raw_string = 0x2000'0000;

enum struct PendingDeclaration : u8 {
    NONE,
    FUNCTION,
    TYPE,
    MODULE,
    LABEL,
};

struct TokenEnd {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] usize next_non_space(std::string_view source, usize position, usize end) {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] usize previous_non_space(std::string_view source, usize position, usize begin) {
    while (position > begin && (source[position - 1] == ' ' || source[position - 1] == '\t')) --position;
    return position;
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.!%^&*+-=/|").contains(value); }

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P');
}

[[nodiscard]] bool all_upper_identifier(std::string_view word) {
    usize letters = 0;
    for (char value : word) {
        if (ascii_alpha(value)) {
            if (ascii_lower(value)) return false;
            ++letters;
        } else if (!ascii_digit(value) && value != '_') {
            return false;
        }
    }
    return letters >= 2;
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    usize digits = 0;
    bool hexadecimal = true;
    if (kind == 'x')
        digits = 2;
    else if (kind == 'u')
        digits = 4;
    else if (kind == 'U')
        digits = 8;
    else if (kind >= '0' && kind <= '7') {
        digits = 2;
        hexadecimal = false;
    }
    const usize limit = std::min(end, position + digits);
    while (position < limit && (hexadecimal ? ascii_hex_digit(source[position]) : source[position] >= '0' && source[position] <= '7'))
        ++position;
    return position;
}

void paint_string_details(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\') {
            const usize escaped_end = escape_end(context.source, position, end);
            paint(context, {.begin = position, .end = escaped_end}, Style::ESCAPE);
            position = escaped_end;
        } else if (context.source[position] == '%' && position + 1 < end && context.source[position + 1] != ' ') {
            usize specifier_end = position + 1;
            while (specifier_end < end && std::string_view("#+- 0123456789.[*]").contains(context.source[specifier_end])) ++specifier_end;
            if (specifier_end < end && std::string_view("vbcdeEfFgGoOpqsTtUuxX").contains(context.source[specifier_end])) {
                paint(context, {.begin = position, .end = specifier_end + 1}, Style::ESCAPE);
                position = specifier_end + 1;
            } else {
                ++position;
            }
        } else {
            ++position;
        }
    }
}

[[nodiscard]] TokenEnd quoted_end(std::string_view source, usize position, usize end, char delimiter) {
    while (position < end) {
        if (source[position] == '\\')
            position = escape_end(source, position, end);
        else if (source[position] == delimiter)
            return {.position = position + 1, .closed = true};
        else
            ++position;
    }
    return {.position = end};
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word) noexcept {
    if (word == "func") return PendingDeclaration::FUNCTION;
    if (word == "type") return PendingDeclaration::TYPE;
    if (word == "package") return PendingDeclaration::MODULE;
    if (word == "goto") return PendingDeclaration::LABEL;
    return PendingDeclaration::NONE;
}

[[nodiscard]] Style declaration_style(PendingDeclaration pending) noexcept {
    switch (pending) {
        case PendingDeclaration::FUNCTION: return Style::FUNCTION;
        case PendingDeclaration::TYPE: return Style::TYPE;
        case PendingDeclaration::MODULE: return Style::MODULE;
        case PendingDeclaration::LABEL: return Style::LABEL;
        case PendingDeclaration::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize begin, usize end, usize line_begin,
                                     usize line_end, PendingDeclaration &pending) {
    if (pending != PendingDeclaration::NONE) {
        const Style result = declaration_style(pending);
        pending = PendingDeclaration::NONE;
        return result;
    }
    if (k_constants.contains(word) || all_upper_identifier(word)) return Style::CONSTANT;
    if (k_types.contains(word)) return Style::TYPE;
    if (k_keywords.contains(word)) {
        pending = declaration_after(word);
        return Style::KEYWORD;
    }
    const usize previous = previous_non_space(source, begin, line_begin);
    const usize next = next_non_space(source, end, line_end);
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (previous > line_begin && source[previous - 1] == '.') return Style::PROPERTY;
    if (next < line_end && source[next] == '.') return Style::MODULE;
    if (next < line_end && source[next] == ':') return Style::PROPERTY;
    if (k_builtins.contains(word)) return Style::FUNCTION;
    if (next < line_end && ascii_identifier_start(source[next])) return Style::PARAMETER;
    if (!word.empty() && ascii_upper(word.front())) return Style::TYPE;
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    usize content_end = end;
    if (content_end > begin && context.source[content_end - 1] == '\n') --content_end;
    if (content_end > begin && context.source[content_end - 1] == '\r') --content_end;

    usize position = begin;
    u32 state = line_state;
    PendingDeclaration pending = PendingDeclaration::NONE;
    while (position < content_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_block_comment) {
            const usize found = context.source.find("*/", position);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) break;
            state = k_normal;
            continue;
        }
        if (kind == k_raw_string) {
            const usize found = context.source.find('`', position);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 1;
            paint(context, {.begin = position, .end = token_end}, Style::RAW_STRING);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) break;
            state = k_normal;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        if (context.source.substr(position).starts_with("//")) {
            paint(context, {.begin = position, .end = content_end}, Style::COMMENT);
            break;
        }
        if (context.source.substr(position).starts_with("/*")) {
            state = k_block_comment;
            continue;
        }
        if (current == '`') {
            const usize found = context.source.find('`', position + 1);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 1;
            paint(context, {.begin = position, .end = token_end}, Style::RAW_STRING);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) state = k_raw_string;
            continue;
        }
        if (current == '"' || current == '\'') {
            const TokenEnd token_end = quoted_end(context.source, position + 1, content_end, current);
            const Style style = current == '"' ? Style::STRING : Style::CHARACTER;
            paint(context, {.begin = position, .end = token_end.position}, style);
            paint_string_details(context, position + 1, token_end.position);
            position = token_end.position;
            continue;
        }
        if (ascii_digit(current) || (current == '.' && position + 1 < content_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < content_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            continue;
        }
        if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            paint(context, {.begin = token_begin, .end = position},
                  identifier_style(context.source, word, token_begin, position, begin, content_end, pending));
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < content_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            continue;
        }
        ++position;
    }

    const u32 kind = state & k_state_mask;
    if (end > content_end && kind == k_block_comment) paint(context, {.begin = content_end, .end = end}, Style::COMMENT);
    if (end > content_end && kind == k_raw_string) paint(context, {.begin = content_end, .end = end}, Style::RAW_STRING);
    return state;
}

} // namespace

void GoLexer::lex(LexContext &context) const {
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
