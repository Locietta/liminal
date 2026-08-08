#include "rust.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexRust.cxx and stlRust.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native port retains nested
// comment and raw-string behavior while removing Scintilla infrastructure,
// generated keyword indices, folding, and macro style IDs. See
// THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = RustLexer::Style;

constexpr auto k_keywords =
    make_word_set("Self", "abstract", "as", "async", "await", "become", "box", "break", "const", "continue", "crate", "do", "dyn", "else",
                  "enum", "extern", "false", "final", "fn", "for", "gen", "if", "impl", "in", "let", "loop", "macro", "match", "mod",
                  "move", "mut", "override", "priv", "pub", "ref", "return", "self", "static", "struct", "super", "trait", "true", "try",
                  "type", "typeof", "union", "unsafe", "unsized", "use", "virtual", "where", "while", "yield");

constexpr auto k_types =
    make_word_set("bool", "c_char", "c_double", "c_float", "c_int", "c_long", "c_longlong", "c_schar", "c_short", "c_uchar", "c_uint",
                  "c_ulong", "c_ulonglong", "c_ushort", "char", "f128", "f16", "f32", "f64", "i128", "i16", "i32", "i64", "i8", "isize",
                  "never", "str", "u128", "u16", "u32", "u64", "u8", "usize");

constexpr auto k_constants = make_word_set("Err", "None", "Ok", "Some");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_state_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_doc_comment = 0x2000'0000;
constexpr u32 k_raw_string = 0x3000'0000;
constexpr u32 k_raw_byte_string = 0x4000'0000;
constexpr u32 k_string = 0x5000'0000;
constexpr u32 k_byte_string = 0x6000'0000;

enum struct PendingDeclaration : u8 {
    NONE,
    STRUCT,
    TRAIT,
    ENUMERATION,
    UNION,
    FUNCTION,
    TYPE,
    CONSTANT,
    MODULE,
};

struct RawStart {
    usize content = 0;
    usize hashes = 0;
    bool bytes = false;
};

struct TokenEnd {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] usize next_non_space(std::string_view source, usize position, usize end) {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
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

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.?~!%^&*+-=/|#@").contains(value); }

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E');
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    if (kind == 'x') {
        const usize limit = std::min(end, position + 2);
        while (position < limit && ascii_hex_digit(source[position])) ++position;
    } else if (kind == 'u' && position < end && source[position] == '{') {
        ++position;
        while (position < end && (ascii_hex_digit(source[position]) || source[position] == '_')) ++position;
        if (position < end && source[position] == '}') ++position;
    }
    return position;
}

void paint_string(LexContext &context, usize begin, usize end, usize content_begin, Style style) {
    paint(context, {.begin = begin, .end = end}, style);
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

[[nodiscard]] TokenEnd string_end(std::string_view source, usize position, usize end) {
    while (position < end) {
        if (source[position] == '\\') {
            position = escape_end(source, position, end);
        } else if (source[position] == '"') {
            return {.position = position + 1, .closed = true};
        } else {
            ++position;
        }
    }
    return {.position = end};
}

[[nodiscard]] RawStart raw_start(std::string_view source, usize position, usize end) {
    bool bytes = false;
    if (position < end && (source[position] == 'b' || source[position] == 'c')) {
        bytes = true;
        ++position;
    }
    if (position >= end || source[position] != 'r') return {};
    ++position;
    const usize hash_begin = position;
    while (position < end && source[position] == '#') ++position;
    if (position >= end || source[position] != '"') return {};
    return {.content = position + 1, .hashes = position - hash_begin, .bytes = bytes};
}

[[nodiscard]] TokenEnd raw_end(std::string_view source, usize position, usize end, usize hashes) {
    for (usize quote = source.find('"', position); quote != std::string_view::npos && quote < end; quote = source.find('"', quote + 1)) {
        usize cursor = quote + 1;
        usize count = 0;
        while (cursor < end && count < hashes && source[cursor] == '#') {
            ++cursor;
            ++count;
        }
        if (count == hashes) return {.position = cursor, .closed = true};
    }
    return {.position = end};
}

[[nodiscard]] TokenEnd character_end(std::string_view source, usize position, usize end) {
    if (position < end && source[position] == '\\')
        position = escape_end(source, position, end);
    else if (position < end)
        ++position;
    return {.position = position < end && source[position] == '\'' ? position + 1 : end,
            .closed = position < end && source[position] == '\''};
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word) noexcept {
    if (word == "struct") return PendingDeclaration::STRUCT;
    if (word == "trait") return PendingDeclaration::TRAIT;
    if (word == "enum") return PendingDeclaration::ENUMERATION;
    if (word == "union") return PendingDeclaration::UNION;
    if (word == "fn") return PendingDeclaration::FUNCTION;
    if (word == "type") return PendingDeclaration::TYPE;
    if (word == "const" || word == "static") return PendingDeclaration::CONSTANT;
    if (word == "mod" || word == "use") return PendingDeclaration::MODULE;
    return PendingDeclaration::NONE;
}

[[nodiscard]] Style declaration_style(PendingDeclaration pending) noexcept {
    switch (pending) {
        case PendingDeclaration::STRUCT: return Style::STRUCT;
        case PendingDeclaration::TRAIT: return Style::TRAIT;
        case PendingDeclaration::ENUMERATION: return Style::ENUMERATION;
        case PendingDeclaration::UNION: return Style::UNION;
        case PendingDeclaration::FUNCTION: return Style::FUNCTION;
        case PendingDeclaration::TYPE: return Style::TYPE;
        case PendingDeclaration::CONSTANT: return Style::CONSTANT;
        case PendingDeclaration::MODULE: return Style::MODULE;
        case PendingDeclaration::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize end, usize line_end, PendingDeclaration &pending,
                                     bool attribute) {
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
    if (next < line_end && source[next] == '!') return Style::MACRO;
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (next + 1 < line_end && source.substr(next, 2) == "::") return Style::MODULE;
    if (!word.empty() && ascii_upper(word.front())) return Style::TYPE;
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 scan_nested_comment(LexContext &context, usize &position, usize end, u32 state) {
    usize depth = state & k_state_payload_mask;
    const Style style = (state & k_state_mask) == k_doc_comment ? Style::DOC_COMMENT : Style::COMMENT;
    const usize token_begin = position;
    while (position < end) {
        if (context.source.substr(position).starts_with("/*")) {
            ++depth;
            position += 2;
        } else if (context.source.substr(position).starts_with("*/")) {
            position += 2;
            if (--depth == 0) {
                paint(context, {.begin = token_begin, .end = position}, style);
                return k_normal;
            }
        } else {
            ++position;
        }
    }
    paint(context, {.begin = token_begin, .end = end}, style);
    return (state & k_state_mask) | static_cast<u32>(depth);
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    usize content_end = end;
    if (content_end > begin && context.source[content_end - 1] == '\n') --content_end;
    if (content_end > begin && context.source[content_end - 1] == '\r') --content_end;

    usize position = begin;
    u32 state = line_state;
    PendingDeclaration pending = PendingDeclaration::NONE;
    usize attribute_depth = 0;

    while (position < content_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_block_comment || kind == k_doc_comment) {
            state = scan_nested_comment(context, position, content_end, state);
            if (state != k_normal) break;
            continue;
        }
        if (kind == k_raw_string || kind == k_raw_byte_string) {
            const usize hashes = state & k_state_payload_mask;
            const TokenEnd token_end = raw_end(context.source, position, content_end, hashes);
            paint(context, {.begin = position, .end = token_end.position}, kind == k_raw_string ? Style::STRING : Style::BYTE_STRING);
            position = token_end.position;
            if (!token_end.closed) break;
            state = k_normal;
            continue;
        }
        if (kind == k_string || kind == k_byte_string) {
            const TokenEnd token_end = string_end(context.source, position, content_end);
            paint_string(context, position, token_end.position, position, kind == k_string ? Style::STRING : Style::BYTE_STRING);
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
        if (context.source.substr(position).starts_with("//")) {
            const bool documentation =
                position + 2 < content_end && (context.source[position + 2] == '/' || context.source[position + 2] == '!');
            paint(context, {.begin = position, .end = content_end}, documentation ? Style::DOC_COMMENT : Style::COMMENT);
            position = content_end;
            break;
        }
        if (context.source.substr(position).starts_with("/*")) {
            const bool documentation =
                position + 2 < content_end && (context.source[position + 2] == '*' || context.source[position + 2] == '!');
            state = documentation ? k_doc_comment : k_block_comment;
            continue;
        }

        const RawStart raw = raw_start(context.source, position, content_end);
        if (raw.content != 0) {
            const TokenEnd token_end = raw_end(context.source, raw.content, content_end, raw.hashes);
            const Style style = raw.bytes ? Style::BYTE_STRING : Style::STRING;
            paint(context, {.begin = position, .end = token_end.position}, style);
            position = token_end.position;
            if (!token_end.closed) state = (raw.bytes ? k_raw_byte_string : k_raw_string) | static_cast<u32>(raw.hashes);
            continue;
        }
        if (current == '"' || ((current == 'b' || current == 'c') && position + 1 < content_end && context.source[position + 1] == '"')) {
            const bool bytes = current != '"';
            const usize quote = position + (bytes ? 1 : 0);
            const TokenEnd token_end = string_end(context.source, quote + 1, content_end);
            paint_string(context, position, token_end.position, quote + 1, bytes ? Style::BYTE_STRING : Style::STRING);
            position = token_end.position;
            if (!token_end.closed) state = bytes ? k_byte_string : k_string;
            continue;
        }
        if (current == '\'' && position + 1 < content_end) {
            const TokenEnd token_end = character_end(context.source, position + 1, content_end);
            if (token_end.closed) {
                paint(context, {.begin = position, .end = token_end.position}, Style::CHARACTER);
                if (context.source[position + 1] == '\\') {
                    paint(context, {.begin = position + 1, .end = escape_end(context.source, position + 1, token_end.position)},
                          Style::ESCAPE);
                }
                position = token_end.position;
            } else if (ascii_identifier_start(context.source[position + 1])) {
                const usize token_begin = position++;
                while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
                paint(context, {.begin = token_begin, .end = position}, Style::LIFETIME);
            } else {
                paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
                ++position;
            }
            continue;
        }
        if (ascii_digit(current)) {
            const usize token_begin = position++;
            while (position < content_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            continue;
        }
        if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const Style style = identifier_style(context.source, word, position, content_end, pending, attribute_depth != 0);
            paint(context, {.begin = token_begin, .end = position}, style);
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            if (current == '#' && position < content_end && context.source[position] == '[') {
                ++position;
                ++attribute_depth;
            } else if (current == '[' && attribute_depth != 0) {
                ++attribute_depth;
            } else if (current == ']' && attribute_depth != 0) {
                --attribute_depth;
            } else {
                while (position < content_end && operator_character(context.source[position]) &&
                       !context.source.substr(position).starts_with("//") && !context.source.substr(position).starts_with("/*")) {
                    if (attribute_depth != 0 && (context.source[position] == '[' || context.source[position] == ']')) break;
                    ++position;
                }
            }
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            continue;
        }
        ++position;
    }

    const u32 kind = state & k_state_mask;
    if (end > content_end) {
        const Style style = kind == k_block_comment   ? Style::COMMENT :
                            kind == k_doc_comment     ? Style::DOC_COMMENT :
                            kind == k_raw_string      ? Style::STRING :
                            kind == k_raw_byte_string ? Style::BYTE_STRING :
                            kind == k_string          ? Style::STRING :
                            kind == k_byte_string     ? Style::BYTE_STRING :
                                                        Style::DEFAULT;
        paint(context, {.begin = content_end, .end = end}, style);
    }
    return state;
}

} // namespace

void RustLexer::lex(LexContext &context) const {
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
