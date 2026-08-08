#include "cpp.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// The classification strategy and language tables are derived from Notepad4's
// LexCPP.cxx and stlCPP.cpp at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0.
// This is a structural C++26 port, not a Scintilla compatibility layer. See
// THIRD_PARTY_NOTICES.md for the upstream BSD-3-Clause and HPND notices.
namespace lighter::lexer {

namespace {

using Style = CppLexer::Style;

constexpr auto k_keywords =
    make_word_set("_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local", "alignas", "alignof",
                  "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept", "audit", "axiom", "bitand", "bitor", "break",
                  "case", "catch", "class", "co_await", "co_return", "co_yield", "compl", "concept", "const", "const_cast", "consteval",
                  "constexpr", "constinit", "continue", "contract_assert", "decltype", "default", "delete", "do", "dynamic_cast", "else",
                  "enum", "explicit", "export", "extern", "false", "final", "for", "friend", "goto", "if", "import", "inline", "interface",
                  "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "override",
                  "private", "protected", "public", "register", "reinterpret_cast", "requires", "restrict", "return", "sizeof", "static",
                  "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
                  "typeid", "typename", "union", "using", "virtual", "volatile", "while", "xor", "xor_eq");

constexpr auto k_types =
    make_word_set("_BitInt", "_Bool", "_Complex", "_Decimal128", "_Decimal32", "_Decimal64", "_Float128", "_Float16", "_Float32",
                  "_Float64", "__auto_type", "__float128", "__fp16", "__int128", "auto", "bool", "byte", "char", "char16_t", "char32_t",
                  "char8_t", "clock_t", "double", "float", "float128_t", "float16_t", "float32_t", "float64_t", "int", "int16_t", "int32_t",
                  "int64_t", "int8_t", "intmax_t", "intptr_t", "isize", "long", "max_align_t", "nullptr_t", "off_t", "ptrdiff_t", "short",
                  "signed", "size_t", "ssize_t", "string", "time_t", "u16", "u32", "u64", "u8", "uint16_t", "uint32_t", "uint64_t",
                  "uint8_t", "uintmax_t", "uintptr_t", "unsigned", "usize", "void", "wchar_t");

constexpr auto k_constants = make_word_set("EOF", "FALSE", "NULL", "TRUE", "false", "nullptr", "true");
constexpr auto k_preprocessor =
    make_word_set("define", "elif", "elifdef", "elifndef", "else", "embed", "endif", "error", "ident", "if", "ifdef", "ifndef", "import",
                  "include", "include_next", "line", "message", "pragma", "region", "sccs", "undef", "using", "warn", "warning");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_state_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_doc_comment = 0x2000'0000;
constexpr u32 k_raw_string = 0x3000'0000;
constexpr u32 k_string = 0x4000'0000;
constexpr u32 k_character = 0x5000'0000;
constexpr u32 k_line_comment = 0x6000'0000;
constexpr u32 k_doc_line_comment = 0x7000'0000;

enum struct PendingDeclaration : u8 {
    NONE,
    CLASS,
    STRUCT,
    UNION,
    ENUMERATION,
    LABEL,
    MODULE,
};

struct LiteralStart {
    usize quote = 0;
    bool raw = false;
    char delimiter = '\0';
};

struct RawStringEnd {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] usize next_non_space(std::string_view source, usize position, usize end) {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) {
        ++position;
    }
    return position;
}

[[nodiscard]] usize previous_non_space(std::string_view source, usize position, usize begin) {
    while (position > begin && (source[position - 1] == ' ' || source[position - 1] == '\t')) {
        --position;
    }
    return position;
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

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.?~!%^&*+-=/|").contains(value); }

[[nodiscard]] bool number_continue(char previous, char current, char next) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '\'') return true;
    if (current == '.') return next != '.';
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P');
}

[[nodiscard]] LiteralStart literal_start(std::string_view source, usize position, usize end) {
    const auto available = source.substr(position, end - position);
    if (available.starts_with("u8R\"")) return {.quote = position + 3, .raw = true, .delimiter = '"'};
    if (available.starts_with("u8\"") || available.starts_with("u8'")) {
        return {.quote = position + 2, .delimiter = source[position + 2]};
    }
    if (available.starts_with("uR\"") || available.starts_with("UR\"") || available.starts_with("LR\"")) {
        return {.quote = position + 2, .raw = true, .delimiter = '"'};
    }
    if (available.starts_with("R\"")) return {.quote = position + 1, .raw = true, .delimiter = '"'};
    if ((available.starts_with("u\"") || available.starts_with("u'") || available.starts_with("U\"") || available.starts_with("U'") ||
         available.starts_with("L\"") || available.starts_with("L'"))) {
        return {.quote = position + 1, .delimiter = source[position + 1]};
    }
    if (!available.empty() && (available.front() == '"' || available.front() == '\'')) {
        return {.quote = position, .delimiter = available.front()};
    }
    return {};
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    if (kind == 'x') {
        while (position < end && ascii_hex_digit(source[position])) ++position;
    } else if (kind == 'u' || kind == 'U') {
        const usize limit = std::min(end, position + (kind == 'u' ? 4 : 8));
        while (position < limit && ascii_hex_digit(source[position])) ++position;
    } else if (kind >= '0' && kind <= '7') {
        const usize limit = std::min(end, position + 2);
        while (position < limit && source[position] >= '0' && source[position] <= '7') ++position;
    }
    return position;
}

[[nodiscard]] bool continued_line(std::string_view source, usize begin, usize end) {
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

[[nodiscard]] usize raw_open_parenthesis(std::string_view source, usize quote, usize end) {
    const usize limit = std::min(end, quote + 18);
    for (usize position = quote + 1; position < limit; ++position) {
        const char value = source[position];
        if (value == '(') return position;
        if (ascii_space(value) || value == '\\' || value == ')') return end;
    }
    return end;
}

[[nodiscard]] RawStringEnd raw_string_end(std::string_view source, usize opener, usize position, usize end) {
    const usize quote = source.find('"', opener);
    if (quote == std::string_view::npos || quote >= source.size()) return {.position = end};
    const usize parenthesis = raw_open_parenthesis(source, quote, source.size());
    if (parenthesis == source.size()) return {.position = end};
    const std::string_view delimiter = source.substr(quote + 1, parenthesis - quote - 1);
    for (usize found = source.find(')', position); found != std::string_view::npos && found < end; found = source.find(')', found + 1)) {
        const usize quote_position = found + 1 + delimiter.size();
        if (quote_position < end && source.substr(found + 1, delimiter.size()) == delimiter && source[quote_position] == '"') {
            return {.position = quote_position + 1, .closed = true};
        }
    }
    return {.position = end};
}

void paint_quoted(LexContext &context, usize begin, usize end, usize escape_begin, Style style) {
    paint(context, {.begin = begin, .end = end}, style);
    for (usize position = escape_begin; position < end;) {
        if (context.source[position] != '\\') {
            ++position;
            continue;
        }
        const usize escaped_end = escape_end(context.source, position, end);
        paint(context, {.begin = position, .end = escaped_end}, Style::ESCAPE);
        position = escaped_end;
    }
}

[[nodiscard]] usize quoted_end(std::string_view source, usize quote, usize end) {
    for (usize position = quote + 1; position < end; ++position) {
        if (source[position] == '\\') {
            position = escape_end(source, position, end) - 1;
        } else if (source[position] == source[quote]) {
            return position + 1;
        }
    }
    return end;
}

[[nodiscard]] Style declaration_style(PendingDeclaration pending) noexcept {
    switch (pending) {
        case PendingDeclaration::CLASS: return Style::CLASS;
        case PendingDeclaration::STRUCT: return Style::STRUCT;
        case PendingDeclaration::UNION: return Style::UNION;
        case PendingDeclaration::ENUMERATION: return Style::ENUMERATION;
        case PendingDeclaration::LABEL: return Style::LABEL;
        case PendingDeclaration::MODULE: return Style::MODULE;
        case PendingDeclaration::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word) noexcept {
    if (word == "class") return PendingDeclaration::CLASS;
    if (word == "struct") return PendingDeclaration::STRUCT;
    if (word == "union") return PendingDeclaration::UNION;
    if (word == "enum") return PendingDeclaration::ENUMERATION;
    if (word == "goto") return PendingDeclaration::LABEL;
    if (word == "import" || word == "module" || word == "namespace") return PendingDeclaration::MODULE;
    return PendingDeclaration::NONE;
}

[[nodiscard]] bool looks_like_type(std::string_view source, std::string_view word, usize next, usize end) {
    if (!word.empty() && ascii_upper(word.front()) && !all_upper_identifier(word)) return true;
    next = next_non_space(source, next, end);
    if (next + 1 < end && source.substr(next, 2) == "::") return true;
    if (next < end && ascii_identifier_start(source[next])) return true;
    if (next < end && (source[next] == '*' || source[next] == '&')) {
        while (next < end && (source[next] == '*' || source[next] == '&' || ascii_space(source[next]))) ++next;
        return next < end && ascii_identifier_start(source[next]);
    }
    return false;
}

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize begin, usize end, usize line_begin,
                                     usize line_end, PendingDeclaration &pending, bool attribute) {
    if (pending != PendingDeclaration::NONE) {
        const Style result = declaration_style(pending);
        pending = PendingDeclaration::NONE;
        return result;
    }
    if (attribute) return Style::ATTRIBUTE;
    if (k_types.contains(word)) return Style::TYPE;
    if (k_constants.contains(word) || all_upper_identifier(word)) return Style::CONSTANT;
    if (k_keywords.contains(word)) {
        pending = declaration_after(word);
        return Style::KEYWORD;
    }

    const usize next = next_non_space(source, end, line_end);
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (word == "std" || (next + 1 < line_end && source.substr(next, 2) == "::")) return Style::MODULE;
    if (looks_like_type(source, word, end, line_end)) return Style::TYPE;

    const usize previous = previous_non_space(source, begin, line_begin);
    if (previous >= 2 && source.substr(previous - 2, 2) == "::") return Style::IDENTIFIER;
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);

    usize content_end = end;
    if (content_end > begin && context.source[content_end - 1] == '\n') --content_end;
    if (content_end > begin && context.source[content_end - 1] == '\r') --content_end;

    usize position = begin;
    u32 state = line_state & k_state_mask;
    PendingDeclaration pending = PendingDeclaration::NONE;
    usize attribute_depth = 0;
    bool visible = false;

    while (position < content_end) {
        if (state == k_block_comment || state == k_doc_comment) {
            const usize found = context.source.find("*/", position);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, state == k_doc_comment ? Style::DOC_COMMENT : Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) break;
            state = k_normal;
            continue;
        }
        if (state == k_raw_string) {
            const usize opener = line_state & k_state_payload_mask;
            const RawStringEnd token_end = raw_string_end(context.source, opener, position, content_end);
            paint(context, {.begin = position, .end = token_end.position}, Style::RAW_STRING);
            position = token_end.position;
            if (!token_end.closed) break;
            state = k_normal;
            continue;
        }
        if (state == k_line_comment || state == k_doc_line_comment) {
            paint(context, {.begin = position, .end = content_end}, state == k_doc_line_comment ? Style::DOC_COMMENT : Style::COMMENT);
            position = content_end;
            break;
        }
        if (state == k_string || state == k_character) {
            const char quote = state == k_string ? '"' : '\'';
            usize token_end = content_end;
            for (usize cursor = position; cursor < content_end; ++cursor) {
                if (context.source[cursor] == '\\') {
                    cursor = escape_end(context.source, cursor, content_end) - 1;
                } else if (context.source[cursor] == quote) {
                    token_end = cursor + 1;
                    state = k_normal;
                    break;
                }
            }
            paint_quoted(context, position, token_end, position, quote == '"' ? Style::STRING : Style::CHARACTER);
            position = token_end;
            if (state != k_normal) break;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        if (context.source.substr(position).starts_with("//")) {
            const bool documentation =
                position + 2 < content_end && (context.source[position + 2] == '/' || context.source[position + 2] == '!');
            paint(context, {.begin = position, .end = content_end}, documentation ? Style::DOC_COMMENT : Style::COMMENT);
            state = documentation ? k_doc_line_comment : k_line_comment;
            position = content_end;
            break;
        }
        if (context.source.substr(position).starts_with("/*")) {
            const bool documentation =
                position + 2 < content_end && (context.source[position + 2] == '*' || context.source[position + 2] == '!');
            const usize found = context.source.find("*/", position + 2);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, documentation ? Style::DOC_COMMENT : Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) state = documentation ? k_doc_comment : k_block_comment;
            visible = true;
            continue;
        }
        if (current == '#' && !visible) {
            paint(context, {.begin = position, .end = position + 1}, Style::PREPROCESSOR);
            position = next_non_space(context.source, position + 1, content_end);
            const usize directive_begin = position;
            while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view directive = context.source.substr(directive_begin, position - directive_begin);
            if (!directive.empty()) paint(context, {.begin = directive_begin, .end = position}, Style::PREPROCESSOR);
            if (k_preprocessor.contains(directive) &&
                (directive == "include" || directive == "include_next" || directive == "import" || directive == "embed")) {
                position = next_non_space(context.source, position, content_end);
                if (position < content_end && (context.source[position] == '<' || context.source[position] == '"')) {
                    const char close = context.source[position] == '<' ? '>' : '"';
                    const usize found = context.source.find(close, position + 1);
                    const usize header_end = found == std::string_view::npos || found >= content_end ? content_end : found + 1;
                    paint(context, {.begin = position, .end = header_end}, Style::HEADER);
                    position = header_end;
                }
            }
            visible = true;
            continue;
        }

        const LiteralStart literal = literal_start(context.source, position, content_end);
        if (literal.delimiter != '\0') {
            if (literal.raw) {
                const usize parenthesis = raw_open_parenthesis(context.source, literal.quote, content_end);
                if (parenthesis != content_end) {
                    const RawStringEnd token_end = raw_string_end(context.source, literal.quote - 1, parenthesis + 1, content_end);
                    paint(context, {.begin = position, .end = token_end.position}, Style::RAW_STRING);
                    if (!token_end.closed) state = k_raw_string | static_cast<u32>((literal.quote - 1) & k_state_payload_mask);
                    position = token_end.position;
                    visible = true;
                    continue;
                }
            } else {
                const usize token_end = quoted_end(context.source, literal.quote, content_end);
                const Style style = literal.delimiter == '"' ? Style::STRING : Style::CHARACTER;
                paint_quoted(context, position, token_end, literal.quote + 1, style);
                if (token_end == content_end && context.source[token_end - 1] != literal.delimiter &&
                    continued_line(context.source, begin, end)) {
                    state = literal.delimiter == '"' ? k_string : k_character;
                }
                position = token_end;
                visible = true;
                continue;
            }
        }

        if (ascii_digit(current) || (current == '.' && position + 1 < content_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < content_end && number_continue(context.source[position - 1], context.source[position],
                                                             position + 1 < content_end ? context.source[position + 1] : '\0')) {
                ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            visible = true;
            continue;
        }
        if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const Style style =
                identifier_style(context.source, word, token_begin, position, begin, content_end, pending, attribute_depth != 0);
            paint(context, {.begin = token_begin, .end = position}, style);
            visible = true;
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            if (current == '[' && position < content_end && context.source[position] == '[') {
                ++position;
                ++attribute_depth;
            } else if (current == ']' && position < content_end && context.source[position] == ']' && attribute_depth != 0) {
                ++position;
                --attribute_depth;
            } else {
                while (position < content_end && operator_character(context.source[position]) &&
                       !context.source.substr(position).starts_with("//") && !context.source.substr(position).starts_with("/*")) {
                    if (attribute_depth != 0 && (context.source[position] == '[' || context.source[position] == ']')) break;
                    ++position;
                }
            }
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            if (current == ';' || current == '{' || current == '}') pending = PendingDeclaration::NONE;
            visible = true;
            continue;
        }
        ++position;
        visible = true;
    }

    if (end > content_end) {
        const Style newline_style = state == k_block_comment    ? Style::COMMENT :
                                    state == k_doc_comment      ? Style::DOC_COMMENT :
                                    state == k_raw_string       ? Style::RAW_STRING :
                                    state == k_line_comment     ? Style::COMMENT :
                                    state == k_doc_line_comment ? Style::DOC_COMMENT :
                                                                  Style::DEFAULT;
        paint(context, {.begin = content_end, .end = end}, newline_style);
    }

    if (state == k_line_comment || state == k_doc_line_comment) {
        return continued_line(context.source, begin, end) ? state : k_normal;
    }
    if ((state == k_string || state == k_character) && !continued_line(context.source, begin, end)) return k_normal;
    return state == k_raw_string ? line_state == k_normal ? state : line_state : state;
}

} // namespace

LanguageInfo CppLexer::language_info() const noexcept {
    switch (dialect) {
        case CppDialect::C: return {.id = "c", .name = "C"};
        case CppDialect::CPP: return {.id = "cpp", .name = "C++"};
        case CppDialect::OBJECTIVE_C: return {.id = "objective-c", .name = "Objective-C"};
        case CppDialect::OBJECTIVE_CPP: return {.id = "objective-cpp", .name = "Objective-C++"};
        case CppDialect::RESOURCE_SCRIPT: return {.id = "resource-script", .name = "Windows Resource Script"};
        case CppDialect::IDL: return {.id = "idl", .name = "IDL/ODL"};
    }
    return {.id = "cpp", .name = "C++"};
}

void CppLexer::lex(LexContext &context) const {
    const auto first_next = std::ranges::upper_bound(context.line_starts, context.range.begin);
    usize line = static_cast<usize>(first_next - context.line_starts.begin() - 1);
    contract_assert(context.line_starts[line] == context.range.begin);

    while (line < context.line_starts.size()) {
        const usize begin = context.line_starts[line];
        const usize end =
            std::min(context.range.end, line + 1 < context.line_starts.size() ? context.line_starts[line + 1] : context.source.size());
        const u32 next_state = lex_line(context, begin, end, context.line_states[line]);
        if (line + 1 < context.line_states.size() && end == context.line_starts[line + 1]) {
            context.line_states[line + 1] = next_state;
        }
        if (end >= context.range.end) break;
        ++line;
    }
}

} // namespace lighter::lexer
