#include "javascript.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexJavaScript.cxx, stlJavaScript.cpp, and
// stlTypeScript.cpp at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0.
// This native port retains dialect configuration, template interpolation,
// regular expressions, documentation comments, and semantic identifiers while
// removing Scintilla accessors, folding, and generated keyword indices. See
// lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = JavaScriptLexer::Style;

constexpr auto k_keywords =
    make_word_set("as", "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
                  "else", "export", "extends", "false", "finally", "for", "from", "function", "get", "if", "import", "in", "instanceof",
                  "let", "new", "null", "of", "return", "set", "static", "super", "switch", "this", "throw", "true", "try", "typeof",
                  "undefined", "var", "void", "while", "with", "yield");

constexpr auto k_typescript_keywords =
    make_word_set("abstract", "accessor", "assert", "asserts", "constructor", "declare", "defer", "enum", "global", "immediate",
                  "implements", "infer", "interface", "intrinsic", "is", "keyof", "namespace", "out", "override", "package", "private",
                  "protected", "public", "readonly", "require", "satisfies", "type", "unique", "using");

constexpr auto k_typescript_types =
    make_word_set("any", "bigint", "boolean", "never", "number", "object", "string", "symbol", "unknown", "void");

constexpr auto k_builtin_types = make_word_set(
    "AggregateError", "Array", "ArrayBuffer", "Atomics", "BigInt", "BigInt64Array", "BigUint64Array", "Boolean", "DataView", "Date",
    "Error", "EvalError", "FinalizationRegistry", "Float16Array", "Float32Array", "Float64Array", "Function", "Int16Array", "Int32Array",
    "Int8Array", "Iterator", "JSON", "Map", "Math", "Number", "Object", "Promise", "Proxy", "RangeError", "ReferenceError", "Reflect",
    "RegExp", "Set", "SharedArrayBuffer", "String", "Symbol", "SyntaxError", "TypeError", "URIError", "Uint16Array", "Uint32Array",
    "Uint8Array", "Uint8ClampedArray", "WeakMap", "WeakRef", "WeakSet");

constexpr auto k_constants = make_word_set("Infinity", "NaN", "false", "null", "true", "undefined");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_state_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_doc_comment = 0x2000'0000;
constexpr u32 k_single_string = 0x3000'0000;
constexpr u32 k_double_string = 0x4000'0000;
constexpr u32 k_template = 0x5000'0000;
constexpr u32 k_template_expression = 0x6000'0000;
constexpr u32 k_resume_expression = 1 << 27;
constexpr u32 k_expression_depth_mask = 0x0000'ffff;

enum struct PendingDeclaration : u8 {
    NONE,
    CLASS,
    INTERFACE,
    ENUMERATION,
    FUNCTION,
    TYPE,
    MODULE,
};

struct TokenEnd {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] bool typescript(JavaScriptDialect dialect) noexcept {
    return dialect == JavaScriptDialect::TYPESCRIPT || dialect == JavaScriptDialect::TSX;
}

[[nodiscard]] bool jsx(JavaScriptDialect dialect) noexcept {
    return dialect == JavaScriptDialect::JSX || dialect == JavaScriptDialect::TSX;
}

[[nodiscard]] usize next_non_space(std::string_view source, usize position, usize end) {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] usize previous_non_space(std::string_view source, usize position, usize begin) {
    while (position > begin && ascii_space(source[position - 1])) --position;
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

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '$'; }

[[nodiscard]] bool identifier_continue(char value) noexcept { return ascii_identifier_continue(value) || value == '$'; }

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|").contains(value); }

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E');
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    usize digits = kind == 'x' ? 2 : (kind == 'u' ? 4 : 0);
    if (kind == 'u' && position < end && source[position] == '{') {
        ++position;
        while (position < end && ascii_hex_digit(source[position])) ++position;
        if (position < end && source[position] == '}') ++position;
        return position;
    }
    const usize limit = std::min(end, position + digits);
    while (position < limit && ascii_hex_digit(source[position])) ++position;
    return position;
}

void paint_escapes(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] != '\\') {
            ++position;
            continue;
        }
        const usize escaped_end = escape_end(context.source, position, end);
        paint(context, {.begin = position, .end = escaped_end}, Style::ESCAPE);
        position = escaped_end;
    }
}

[[nodiscard]] TokenEnd quoted_end(std::string_view source, usize position, usize end, char delimiter) {
    while (position < end) {
        if (source[position] == '\\') {
            position = escape_end(source, position, end);
        } else if (source[position] == delimiter) {
            return {.position = position + 1, .closed = true};
        } else {
            ++position;
        }
    }
    return {.position = end};
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

[[nodiscard]] u32 expression_depth(u32 state) noexcept { return state & k_expression_depth_mask; }

[[nodiscard]] u32 resume_expression(u32 state) noexcept {
    return (state & k_resume_expression) != 0 ? k_template_expression | expression_depth(state) : k_normal;
}

[[nodiscard]] u32 suspended_state(u32 kind, u32 state) noexcept {
    if ((state & k_state_mask) != k_template_expression) return kind;
    return kind | k_resume_expression | expression_depth(state);
}

void paint_doc_tags(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] != '@' || position + 1 >= end || !identifier_start(context.source[position + 1])) {
            ++position;
            continue;
        }
        const usize tag_begin = position++;
        while (position < end && identifier_continue(context.source[position])) ++position;
        paint(context, {.begin = tag_begin, .end = position}, Style::DOC_TAG);
    }
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word, bool is_typescript) noexcept {
    if (word == "class") return PendingDeclaration::CLASS;
    if (word == "function") return PendingDeclaration::FUNCTION;
    if (word == "import" || word == "namespace") return PendingDeclaration::MODULE;
    if (!is_typescript) return PendingDeclaration::NONE;
    if (word == "interface") return PendingDeclaration::INTERFACE;
    if (word == "enum") return PendingDeclaration::ENUMERATION;
    if (word == "type") return PendingDeclaration::TYPE;
    return PendingDeclaration::NONE;
}

[[nodiscard]] Style declaration_style(PendingDeclaration pending) noexcept {
    switch (pending) {
        case PendingDeclaration::CLASS:
        case PendingDeclaration::TYPE: return Style::CLASS;
        case PendingDeclaration::INTERFACE: return Style::INTERFACE;
        case PendingDeclaration::ENUMERATION: return Style::ENUMERATION;
        case PendingDeclaration::FUNCTION: return Style::FUNCTION;
        case PendingDeclaration::MODULE: return Style::MODULE;
        case PendingDeclaration::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize begin, usize end, usize line_begin,
                                     usize line_end, bool is_typescript, PendingDeclaration &pending, bool decorator) {
    if (pending != PendingDeclaration::NONE) {
        const Style result = declaration_style(pending);
        pending = PendingDeclaration::NONE;
        return result;
    }
    if (decorator) return Style::DECORATOR;
    if (k_constants.contains(word) || all_upper_identifier(word)) return Style::CONSTANT;
    if (k_builtin_types.contains(word) || (is_typescript && k_typescript_types.contains(word))) return Style::TYPE;
    if (k_keywords.contains(word) || (is_typescript && k_typescript_keywords.contains(word))) {
        pending = declaration_after(word, is_typescript);
        return Style::KEYWORD;
    }

    const usize previous = previous_non_space(source, begin, line_begin);
    const usize next = next_non_space(source, end, line_end);
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (previous > line_begin && source[previous - 1] == '.') return Style::PROPERTY;
    if (next < line_end && source[next] == ':') return Style::PROPERTY;
    if (!word.empty() && ascii_upper(word.front())) return Style::TYPE;
    return Style::IDENTIFIER;
}

[[nodiscard]] bool keyword_allows_regex(std::string_view word) noexcept {
    return word == "await" || word == "case" || word == "delete" || word == "do" || word == "else" || word == "in" || word == "of" ||
           word == "return" || word == "throw" || word == "typeof" || word == "void" || word == "yield";
}

[[nodiscard]] bool regex_allowed_before(std::string_view source, usize position) {
    position = previous_non_space(source, position, 0);
    if (position == 0) return true;
    const char previous = source[position - 1];
    return previous != ')' && previous != ']' && previous != '}' && previous != '\'' && previous != '"' && previous != '`' &&
           !identifier_continue(previous) && !ascii_digit(previous);
}

[[nodiscard]] TokenEnd regex_end(std::string_view source, usize position, usize end) {
    bool character_class = false;
    while (position < end) {
        const char current = source[position];
        if (current == '\\') {
            position = std::min(end, position + 2);
        } else if (current == '[') {
            character_class = true;
            ++position;
        } else if (current == ']' && character_class) {
            character_class = false;
            ++position;
        } else if (current == '/' && !character_class) {
            ++position;
            while (position < end && ascii_alpha(source[position])) ++position;
            return {.position = position, .closed = true};
        } else {
            ++position;
        }
    }
    return {.position = end};
}

[[nodiscard]] bool jsx_name_continue(char value) noexcept {
    return identifier_continue(value) || value == '-' || value == ':' || value == '.';
}

[[nodiscard]] usize scan_jsx_tag(LexContext &context, usize position, usize end) {
    const usize opening_end = position + 1 < end && context.source[position + 1] == '/' ? position + 2 : position + 1;
    paint(context, {.begin = position, .end = opening_end}, Style::OPERATOR);
    position = opening_end;

    if (position < end && identifier_start(context.source[position])) {
        const usize tag_begin = position++;
        while (position < end && jsx_name_continue(context.source[position])) ++position;
        paint(context, {.begin = tag_begin, .end = position}, Style::TYPE);
    }

    while (position < end) {
        if (context.source.substr(position).starts_with("/>")) {
            paint(context, {.begin = position, .end = position + 2}, Style::OPERATOR);
            return position + 2;
        }
        if (context.source[position] == '>') {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            return position + 1;
        }
        if (context.source[position] == '\'' || context.source[position] == '"') {
            const TokenEnd token_end = quoted_end(context.source, position + 1, end, context.source[position]);
            paint(context, {.begin = position, .end = token_end.position}, Style::STRING);
            paint_escapes(context, position + 1, token_end.position);
            position = token_end.position;
            continue;
        }
        if (identifier_start(context.source[position])) {
            const usize attribute_begin = position++;
            while (position < end && jsx_name_continue(context.source[position])) ++position;
            paint(context, {.begin = attribute_begin, .end = position}, Style::PROPERTY);
            continue;
        }
        if (operator_character(context.source[position])) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
        }
        ++position;
    }
    return position;
}

[[nodiscard]] u32 scan_template(LexContext &context, usize &position, usize end, u32 state) {
    const usize token_begin = position;
    while (position < end) {
        if (context.source[position] == '\\') {
            position = escape_end(context.source, position, end);
        } else if (context.source[position] == '`') {
            ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::TEMPLATE);
            paint_escapes(context, token_begin, position);
            return resume_expression(state);
        } else if (context.source[position] == '$' && position + 1 < end && context.source[position + 1] == '{' &&
                   (state & k_resume_expression) == 0) {
            paint(context, {.begin = token_begin, .end = position}, Style::TEMPLATE);
            paint_escapes(context, token_begin, position);
            paint(context, {.begin = position, .end = position + 2}, Style::OPERATOR);
            position += 2;
            return k_template_expression | 1;
        } else {
            ++position;
        }
    }
    paint(context, {.begin = token_begin, .end = end}, Style::TEMPLATE);
    paint_escapes(context, token_begin, end);
    return state;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state, JavaScriptDialect dialect) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    usize content_end = end;
    if (content_end > begin && context.source[content_end - 1] == '\n') --content_end;
    if (content_end > begin && context.source[content_end - 1] == '\r') --content_end;

    const bool is_typescript = typescript(dialect);
    usize position = begin;
    u32 state = line_state;
    PendingDeclaration pending = PendingDeclaration::NONE;
    bool decorator = false;
    bool can_start_regex = regex_allowed_before(context.source, begin);

    while (position < content_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_block_comment || kind == k_doc_comment) {
            const usize found = context.source.find("*/", position);
            const usize token_end = found == std::string_view::npos || found >= content_end ? content_end : found + 2;
            const Style style = kind == k_doc_comment ? Style::DOC_COMMENT : Style::COMMENT;
            paint(context, {.begin = position, .end = token_end}, style);
            if (kind == k_doc_comment) paint_doc_tags(context, position, token_end);
            position = token_end;
            if (found == std::string_view::npos || found >= content_end) break;
            state = resume_expression(state);
            can_start_regex = false;
            continue;
        }
        if (kind == k_single_string || kind == k_double_string) {
            const char delimiter = kind == k_single_string ? '\'' : '"';
            const TokenEnd token_end = quoted_end(context.source, position, content_end, delimiter);
            paint(context, {.begin = position, .end = token_end.position}, Style::STRING);
            paint_escapes(context, position, token_end.position);
            position = token_end.position;
            if (!token_end.closed) break;
            state = resume_expression(state);
            can_start_regex = false;
            continue;
        }
        if (kind == k_template) {
            state = scan_template(context, position, content_end, state);
            can_start_regex = false;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        if (position == 0 && context.source.substr(position).starts_with("#!")) {
            paint(context, {.begin = position, .end = content_end}, Style::COMMENT);
            position = content_end;
            break;
        }
        if (context.source.substr(position).starts_with("//")) {
            const bool documentation = position + 2 < content_end && context.source[position + 2] == '/';
            paint(context, {.begin = position, .end = content_end}, documentation ? Style::DOC_COMMENT : Style::COMMENT);
            if (documentation) paint_doc_tags(context, position, content_end);
            position = content_end;
            break;
        }
        if (context.source.substr(position).starts_with("/*")) {
            const bool documentation = position + 2 < content_end && context.source[position + 2] == '*';
            state = suspended_state(documentation ? k_doc_comment : k_block_comment, state);
            continue;
        }
        if (current == '\'' || current == '"') {
            const TokenEnd token_end = quoted_end(context.source, position + 1, content_end, current);
            paint(context, {.begin = position, .end = token_end.position}, Style::STRING);
            paint_escapes(context, position + 1, token_end.position);
            position = token_end.position;
            if (!token_end.closed && line_continues(context.source, begin, end)) {
                state = suspended_state(current == '\'' ? k_single_string : k_double_string, state);
            }
            can_start_regex = false;
            decorator = false;
            continue;
        }
        if (current == '`') {
            const u32 template_state = suspended_state(k_template, state);
            paint(context, {.begin = position, .end = position + 1}, Style::TEMPLATE);
            ++position;
            state = scan_template(context, position, content_end, template_state);
            can_start_regex = false;
            decorator = false;
            continue;
        }
        if (current == '/' && can_start_regex && position + 1 < content_end && context.source[position + 1] != '=') {
            const TokenEnd token_end = regex_end(context.source, position + 1, content_end);
            if (token_end.closed) {
                paint(context, {.begin = position, .end = token_end.position}, Style::REGEX);
                paint_escapes(context, position + 1, token_end.position);
                position = token_end.position;
                can_start_regex = false;
                decorator = false;
                continue;
            }
        }
        if (current == '<' && jsx(dialect) && position + 1 < content_end &&
            (context.source[position + 1] == '/' || (can_start_regex && identifier_start(context.source[position + 1])))) {
            position = scan_jsx_tag(context, position, content_end);
            can_start_regex = false;
            decorator = false;
            continue;
        }
        if (ascii_digit(current) || (current == '.' && position + 1 < content_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < content_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            can_start_regex = false;
            decorator = false;
            continue;
        }
        if (identifier_start(current)) {
            const usize token_begin = position++;
            while (position < content_end && identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const Style style =
                identifier_style(context.source, word, token_begin, position, begin, content_end, is_typescript, pending, decorator);
            paint(context, {.begin = token_begin, .end = position}, style);
            can_start_regex = style == Style::KEYWORD && keyword_allows_regex(word);
            decorator = false;
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            if (kind == k_template_expression && current == '{') {
                state = k_template_expression | std::min<u32>(expression_depth(state) + 1, k_expression_depth_mask);
            } else if (kind == k_template_expression && current == '}') {
                const u32 depth = expression_depth(state);
                state = depth <= 1 ? k_template : k_template_expression | (depth - 1);
            }
            while (position < content_end && operator_character(context.source[position])) {
                if ((state & k_state_mask) == k_template || context.source[position] == '{' || context.source[position] == '}') break;
                ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            decorator = current == '@';
            can_start_regex = current != ')' && current != ']' && current != '}' && current != '.';
            continue;
        }
        ++position;
        decorator = false;
    }

    const u32 kind = state & k_state_mask;
    if (end > content_end) {
        if (kind == k_block_comment || kind == k_doc_comment) {
            paint(context, {.begin = content_end, .end = end}, kind == k_doc_comment ? Style::DOC_COMMENT : Style::COMMENT);
        } else if (kind == k_template) {
            paint(context, {.begin = content_end, .end = end}, Style::TEMPLATE);
        } else if ((kind == k_single_string || kind == k_double_string) && line_continues(context.source, begin, end)) {
            paint(context, {.begin = content_end, .end = end}, Style::STRING);
        }
    }
    if ((kind == k_single_string || kind == k_double_string) && !line_continues(context.source, begin, end)) {
        return resume_expression(state);
    }
    return state;
}

} // namespace

LanguageInfo JavaScriptLexer::language_info() const noexcept {
    switch (dialect) {
        case JavaScriptDialect::JAVASCRIPT: return {.id = "javascript", .name = "JavaScript"};
        case JavaScriptDialect::JSX: return {.id = "jsx", .name = "JavaScript JSX"};
        case JavaScriptDialect::TYPESCRIPT: return {.id = "typescript", .name = "TypeScript"};
        case JavaScriptDialect::TSX: return {.id = "tsx", .name = "TypeScript TSX"};
    }
    return {.id = "javascript", .name = "JavaScript"};
}

void JavaScriptLexer::lex(LexContext &context) const {
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
