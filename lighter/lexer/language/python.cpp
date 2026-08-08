#include "python.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexPython.cxx and stlPython.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. This port keeps the language
// classification and multiline-state ideas while replacing the Scintilla
// document, generated keyword metadata, folding, and numeric style API. See
// lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = PythonLexer::Style;

constexpr auto k_keywords =
    make_word_set("False", "None", "True", "and", "as", "assert", "async", "await", "break", "case", "class", "continue", "def", "del",
                  "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "match", "nonlocal",
                  "not", "or", "pass", "raise", "return", "try", "type", "while", "with", "yield");

constexpr auto k_types = make_word_set("Any", "Callable", "ClassVar", "Final", "Generic", "Iterable", "Iterator", "Literal", "Mapping",
                                       "NamedTuple", "Never", "NoReturn", "Optional", "Protocol", "Self", "Sequence", "TypeAlias",
                                       "TypeVar", "TypedDict", "Union", "bool", "bytearray", "bytes", "complex", "dict", "float",
                                       "frozenset", "int", "list", "memoryview", "object", "range", "set", "slice", "str", "tuple", "type");

constexpr auto k_builtins = make_word_set(
    "__build_class__", "__import__", "abs", "aiter", "all", "anext", "any", "ascii", "bin", "breakpoint", "callable", "chr", "classmethod",
    "compile", "delattr", "dir", "divmod", "enumerate", "eval", "exec", "filter", "format", "getattr", "globals", "hasattr", "hash", "help",
    "hex", "id", "input", "isinstance", "issubclass", "iter", "len", "locals", "map", "max", "min", "next", "oct", "open", "ord", "pow",
    "print", "property", "repr", "reversed", "round", "setattr", "sorted", "staticmethod", "sum", "super", "vars", "zip");

constexpr auto k_constants = make_word_set("Ellipsis", "False", "None", "NotImplemented", "True", "__debug__");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_state_flags = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_single = 0x1000'0000;
constexpr u32 k_double = 0x2000'0000;
constexpr u32 k_triple_single = 0x3000'0000;
constexpr u32 k_triple_double = 0x4000'0000;
constexpr u32 k_raw = 1 << 0;
constexpr u32 k_format = 1 << 1;

enum struct PendingDeclaration : u8 {
    NONE,
    CLASS,
    FUNCTION,
};

struct StringStart {
    usize quote = 0;
    usize content = 0;
    char delimiter = '\0';
    bool triple = false;
    bool raw = false;
    bool format = false;
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

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@~!%^&*+-=/|").contains(value); }

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E');
}

[[nodiscard]] bool string_prefix_character(char value) noexcept {
    value = ascii_to_lower(value);
    return value == 'r' || value == 'b' || value == 'u' || value == 'f';
}

[[nodiscard]] StringStart string_start(std::string_view source, usize position, usize end) {
    usize quote = position;
    while (quote < end && quote - position < 2 && string_prefix_character(source[quote])) ++quote;
    if (quote >= end || (source[quote] != '\'' && source[quote] != '"')) return {};

    bool raw = false;
    bool format = false;
    for (usize cursor = position; cursor < quote; ++cursor) {
        const char prefix = ascii_to_lower(source[cursor]);
        raw |= prefix == 'r';
        format |= prefix == 'f';
    }
    const char delimiter = source[quote];
    const bool triple = quote + 2 < end && source[quote + 1] == delimiter && source[quote + 2] == delimiter;
    return {
        .quote = quote,
        .content = quote + (triple ? 3 : 1),
        .delimiter = delimiter,
        .triple = triple,
        .raw = raw,
        .format = format,
    };
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    usize count = 0;
    if (kind == 'x')
        count = 2;
    else if (kind == 'u')
        count = 4;
    else if (kind == 'U')
        count = 8;
    else if (kind >= '0' && kind <= '7')
        count = 2;
    const usize limit = std::min(end, position + count);
    while (position < limit && (ascii_hex_digit(source[position]) || (kind >= '0' && kind <= '7' && source[position] <= '7'))) ++position;
    return position;
}

[[nodiscard]] TokenEnd string_end(std::string_view source, usize position, usize end, char delimiter, bool triple, bool raw) {
    while (position < end) {
        if (!raw && source[position] == '\\') {
            position = escape_end(source, position, end);
        } else if (triple && position + 2 < end && source[position] == delimiter && source[position + 1] == delimiter &&
                   source[position + 2] == delimiter) {
            return {.position = position + 3, .closed = true};
        } else if (!triple && source[position] == delimiter) {
            return {.position = position + 1, .closed = true};
        } else {
            ++position;
        }
    }
    return {.position = end};
}

[[nodiscard]] Style string_style(bool raw, bool format) noexcept {
    if (format) return Style::FORMAT_STRING;
    if (raw) return Style::RAW_STRING;
    return Style::STRING;
}

void paint_string(LexContext &context, usize begin, usize end, usize content_begin, Style style, bool raw) {
    paint(context, {.begin = begin, .end = end}, style);
    if (raw) return;
    for (usize position = content_begin; position < end;) {
        if (context.source[position] != '\\') {
            ++position;
            continue;
        }
        const usize escaped_end = escape_end(context.source, position, end);
        paint(context, {.begin = position, .end = escaped_end}, Style::ESCAPE);
        position = escaped_end;
    }
}

[[nodiscard]] bool line_continues(std::string_view source, usize begin, usize end) {
    usize position = end;
    if (position > begin && source[position - 1] == '\n') --position;
    if (position > begin && source[position - 1] == '\r') --position;
    usize slashes = 0;
    while (position > begin && source[position - 1] == '\\') {
        --position;
        ++slashes;
    }
    return (slashes & 1) != 0;
}

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize begin, usize end, usize line_begin,
                                     usize line_end, PendingDeclaration &pending, bool decorator) {
    if (pending != PendingDeclaration::NONE) {
        const Style result = pending == PendingDeclaration::CLASS ? Style::CLASS : Style::FUNCTION;
        pending = PendingDeclaration::NONE;
        return result;
    }
    if (decorator) return Style::DECORATOR;
    if (k_constants.contains(word)) return Style::CONSTANT;
    if (k_types.contains(word)) return Style::TYPE;
    if (k_builtins.contains(word)) return Style::BUILTIN;
    if (k_keywords.contains(word)) {
        if (word == "class")
            pending = PendingDeclaration::CLASS;
        else if (word == "def")
            pending = PendingDeclaration::FUNCTION;
        return Style::KEYWORD;
    }

    const usize previous = previous_non_space(source, begin, line_begin);
    if (previous > line_begin && source[previous - 1] == '.') return Style::PROPERTY;
    const usize next = next_non_space(source, end, line_end);
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (!word.empty() && ascii_upper(word.front())) return Style::TYPE;
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 encode_state(const StringStart &start) noexcept {
    const u32 kind =
        start.triple ? (start.delimiter == '\'' ? k_triple_single : k_triple_double) : (start.delimiter == '\'' ? k_single : k_double);
    return kind | (start.raw ? k_raw : 0) | (start.format ? k_format : 0);
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    usize content_end = end;
    if (content_end > begin && context.source[content_end - 1] == '\n') --content_end;
    if (content_end > begin && context.source[content_end - 1] == '\r') --content_end;

    usize position = begin;
    u32 state = line_state;
    PendingDeclaration pending = PendingDeclaration::NONE;
    bool decorator = false;

    while (position < content_end) {
        const u32 kind = state & k_state_mask;
        if (kind != k_normal) {
            const bool triple = kind == k_triple_single || kind == k_triple_double;
            const char delimiter = kind == k_single || kind == k_triple_single ? '\'' : '"';
            const bool raw = (state & k_raw) != 0;
            const bool format = (state & k_format) != 0;
            const TokenEnd token_end = string_end(context.source, position, content_end, delimiter, triple, raw);
            paint_string(context, position, token_end.position, position, string_style(raw, format), raw);
            position = token_end.position;
            if (!token_end.closed) break;
            state = k_normal;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        if (current == '#') {
            paint(context, {.begin = position, .end = content_end}, Style::COMMENT);
            position = content_end;
            break;
        }

        const StringStart string = string_start(context.source, position, content_end);
        if (string.delimiter != '\0') {
            const TokenEnd token_end = string_end(context.source, string.content, content_end, string.delimiter, string.triple, string.raw);
            paint_string(context, position, token_end.position, string.content, string_style(string.raw, string.format), string.raw);
            position = token_end.position;
            if (!token_end.closed && (string.triple || line_continues(context.source, begin, end))) state = encode_state(string);
            decorator = false;
            continue;
        }
        if (ascii_digit(current) || (current == '.' && position + 1 < content_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < content_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            decorator = false;
            continue;
        }
        if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const Style style = identifier_style(context.source, word, token_begin, position, begin, content_end, pending, decorator);
            paint(context, {.begin = token_begin, .end = position}, style);
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            if (current == '@')
                decorator = true;
            else if (current != '.')
                decorator = false;
            while (position < content_end && operator_character(context.source[position]) && context.source[position] != '@' &&
                   context.source[position] != '.') {
                ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            continue;
        }
        ++position;
        decorator = false;
    }

    const u32 kind = state & k_state_mask;
    if (end > content_end && kind != k_normal) {
        const bool raw = (state & k_raw) != 0;
        const bool format = (state & k_format) != 0;
        paint(context, {.begin = content_end, .end = end}, string_style(raw, format));
    }
    if ((kind == k_single || kind == k_double) && !line_continues(context.source, begin, end)) return k_normal;
    return state & (k_state_mask | k_state_flags);
}

} // namespace

void PythonLexer::lex(LexContext &context) const {
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
