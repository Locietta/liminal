#include "brace.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexJava.cxx, LexCSharp.cxx, LexD.cxx, LexDart.cxx,
// LexCangjie.cxx, LexGroovy.cxx, LexHaxe.cxx, LexKotlin.cxx, LexScala.cxx,
// LexSwift.cxx, LexZig.cxx, LexAsymptote.cxx, and the matching stl*.cpp files
// at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. This data-oriented
// family retains distinct language identities, keyword/type classification,
// nested comments, annotations, multiline strings, interpolation, and
// declaration roles without reproducing Scintilla lexer classes or folding.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = BraceDialect;
using Style = BraceLexer::Style;

constexpr auto k_common_keywords = make_word_set(
    "abstract", "as", "assert", "async", "await", "break", "case", "catch", "class", "const", "continue", "default", "defer", "do", "else",
    "enum", "export", "extends", "extension", "external", "final", "finally", "fn", "for", "foreach", "from", "func", "function", "goto",
    "if", "implements", "import", "in", "interface", "internal", "is", "let", "macro", "mixin", "module", "namespace", "native", "new",
    "operator", "override", "package", "private", "protected", "protocol", "public", "record", "requires", "return", "sealed", "static",
    "struct", "super", "switch", "synchronized", "this", "throw", "throws", "trait", "try", "type", "typealias", "typeof", "using", "val",
    "var", "virtual", "void", "volatile", "when", "where", "while", "with", "yield");
constexpr auto k_java_keywords = make_word_set("exports", "instanceof", "non-sealed", "open", "opens", "permits", "provides", "strictfp",
                                               "to", "transient", "transitive", "uses");
constexpr auto k_csharp_keywords = make_word_set(
    "add", "alias", "and", "ascending", "base", "by", "checked", "descending", "dynamic", "equals", "event", "explicit", "extern", "file",
    "fixed", "global", "implicit", "into", "join", "lock", "nameof", "not", "notnull", "on", "or", "orderby", "out", "params", "partial",
    "ref", "remove", "required", "scoped", "select", "set", "sizeof", "stackalloc", "unchecked", "unmanaged", "unsafe", "value");
constexpr auto k_d_keywords = make_word_set("__gshared", "__traits", "alias", "align", "body", "cast", "debug", "immutable", "inout",
                                            "lazy", "nothrow", "pragma", "pure", "scope", "shared", "template", "unittest");
constexpr auto k_dart_keywords =
    make_word_set("augment", "covariant", "factory", "get", "late", "library", "part", "rethrow", "set", "show", "sync", "typedef");
constexpr auto k_cangjie_keywords = make_word_set("catch", "extend", "foreign", "init", "main", "mut", "prop", "quote", "spawn");
constexpr auto k_groovy_keywords = make_word_set("def", "delegate", "property", "threadsafe");
constexpr auto k_haxe_keywords = make_word_set("cast", "inline", "overload", "untyped");
constexpr auto k_kotlin_keywords = make_word_set("actual", "annotation", "companion", "constructor", "crossinline", "data", "expect", "fun",
                                                 "infix", "init", "lateinit", "noinline", "object", "reified", "suspend", "tailrec");
constexpr auto k_scala_keywords = make_word_set("def", "derives", "end", "enum", "given", "implicit", "infix", "lazy", "match", "object",
                                                "opaque", "then", "transparent");
constexpr auto k_swift_keywords = make_word_set("actor", "associatedtype", "convenience", "didSet", "distributed", "fileprivate",
                                                "indirect", "inout", "mutating", "nonisolated", "required", "some", "subscript", "willSet");
constexpr auto k_zig_keywords = make_word_set("addrspace", "align", "allowzero", "anyframe", "anytype", "asm", "callconv", "comptime",
                                              "errdefer", "error", "export", "linksection", "noalias", "nosuspend", "opaque", "or",
                                              "orelse", "packed", "pub", "resume", "suspend", "test", "threadlocal", "unreachable");
constexpr auto k_asymptote_keywords = make_word_set("access", "controls", "cycle", "newframe", "operator", "restricted", "tension");

constexpr auto k_common_types =
    make_word_set("Any", "Array", "Bool", "Boolean", "Byte", "Character", "Double", "Dynamic", "Float", "Int", "Integer", "List", "Long",
                  "Map", "Never", "Nothing", "Object", "Set", "Short", "String", "UInt", "Unit", "Void", "any", "bool", "byte", "char",
                  "decimal", "double", "dynamic", "float", "int", "long", "object", "short", "string", "uint", "ulong", "ushort");
constexpr auto k_d_types =
    make_word_set("cdouble", "cent", "cfloat", "creal", "dchar", "idouble", "ifloat", "ireal", "real", "ubyte", "ucent", "wchar");
constexpr auto k_dart_types =
    make_word_set("Future", "Iterable", "Never", "Null", "Object", "Record", "RuneIterator", "Stream", "Symbol", "Type", "double", "num");
constexpr auto k_swift_types =
    make_word_set("Character", "Dictionary", "Optional", "Result", "Self", "Substring", "Unicode", "any", "some");
constexpr auto k_zig_types =
    make_word_set("anyerror", "anyopaque", "c_int", "c_long", "comptime_float", "comptime_int", "f128", "f16", "f32", "f64", "f80", "i128",
                  "i16", "i32", "i64", "i8", "isize", "noreturn", "type", "u128", "u16", "u32", "u64", "u8", "usize");
constexpr auto k_constants = make_word_set("None", "false", "nil", "null", "this", "true", "undefined");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_state_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_doc_comment = 0x2000'0000;
constexpr u32 k_nested_comment = 0x3000'0000;
constexpr u32 k_triple_double = 0x4000'0000;
constexpr u32 k_triple_single = 0x5000'0000;
constexpr u32 k_backtick_string = 0x6000'0000;

enum struct PendingDeclaration : u8 {
    NONE,
    TYPE,
    FUNCTION,
    MODULE,
    PROPERTY,
};

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

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@#?~!%^&*+-=/|\\").contains(value); }

[[nodiscard]] bool keyword(std::string_view word, Dialect dialect) noexcept {
    if (k_common_keywords.contains(word)) return true;
    switch (dialect) {
        case Dialect::JAVA: return k_java_keywords.contains(word);
        case Dialect::CSHARP: return k_csharp_keywords.contains(word);
        case Dialect::D: return k_d_keywords.contains(word);
        case Dialect::DART: return k_dart_keywords.contains(word);
        case Dialect::CANGJIE: return k_cangjie_keywords.contains(word);
        case Dialect::GROOVY:
        case Dialect::GRADLE: return k_groovy_keywords.contains(word);
        case Dialect::HAXE: return k_haxe_keywords.contains(word);
        case Dialect::KOTLIN: return k_kotlin_keywords.contains(word);
        case Dialect::SCALA: return k_scala_keywords.contains(word);
        case Dialect::SWIFT: return k_swift_keywords.contains(word);
        case Dialect::ZIG: return k_zig_keywords.contains(word);
        case Dialect::ASYMPTOTE: return k_asymptote_keywords.contains(word);
    }
    return false;
}

[[nodiscard]] bool builtin_type(std::string_view word, Dialect dialect) noexcept {
    if (k_common_types.contains(word)) return true;
    switch (dialect) {
        case Dialect::D: return k_d_types.contains(word);
        case Dialect::DART: return k_dart_types.contains(word);
        case Dialect::SWIFT: return k_swift_types.contains(word);
        case Dialect::ZIG: return k_zig_types.contains(word);
        default: return false;
    }
}

[[nodiscard]] bool nested_comments(Dialect dialect) noexcept { return dialect == Dialect::D || dialect == Dialect::SWIFT; }

[[nodiscard]] bool triple_strings(Dialect dialect) noexcept {
    return dialect == Dialect::JAVA || dialect == Dialect::CSHARP || dialect == Dialect::DART || dialect == Dialect::GROOVY ||
           dialect == Dialect::GRADLE || dialect == Dialect::KOTLIN || dialect == Dialect::SCALA || dialect == Dialect::SWIFT;
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word) noexcept {
    if (word == "class" || word == "enum" || word == "interface" || word == "mixin" || word == "protocol" || word == "record" ||
        word == "struct" || word == "trait" || word == "type" || word == "typealias")
        return PendingDeclaration::TYPE;
    if (word == "def" || word == "fn" || word == "fun" || word == "func" || word == "function") return PendingDeclaration::FUNCTION;
    if (word == "import" || word == "module" || word == "namespace" || word == "package") return PendingDeclaration::MODULE;
    if (word == "const" || word == "let" || word == "val" || word == "var") return PendingDeclaration::PROPERTY;
    return PendingDeclaration::NONE;
}

[[nodiscard]] Style declaration_style(PendingDeclaration pending) noexcept {
    switch (pending) {
        case PendingDeclaration::TYPE: return Style::TYPE;
        case PendingDeclaration::FUNCTION: return Style::FUNCTION;
        case PendingDeclaration::MODULE: return Style::MODULE;
        case PendingDeclaration::PROPERTY: return Style::PROPERTY;
        case PendingDeclaration::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) noexcept {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    usize digits = kind == 'x' ? 2 : kind == 'u' ? 4 : kind == 'U' ? 8 : 0;
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

void paint_string_details(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\') {
            const usize token_end = escape_end(context.source, position, end);
            paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
            position = token_end;
        } else if (context.source[position] == '$' && position + 1 < end) {
            const usize token_begin = position++;
            if (context.source[position] == '{') {
                usize depth = 1;
                ++position;
                while (position < end && depth != 0) {
                    if (context.source[position] == '{')
                        ++depth;
                    else if (context.source[position] == '}')
                        --depth;
                    ++position;
                }
                paint(context, {.begin = token_begin, .end = position}, Style::PARAMETER);
            } else if (ascii_identifier_start(context.source[position])) {
                ++position;
                while (position < end && ascii_identifier_continue(context.source[position])) ++position;
                paint(context, {.begin = token_begin, .end = position}, Style::PROPERTY);
            }
        } else if (context.source[position] == '%' && position + 1 < end) {
            usize token_end = position + 1;
            while (token_end < end && std::string_view("#+- 0123456789.*").contains(context.source[token_end])) ++token_end;
            if (token_end < end && ascii_alpha(context.source[token_end])) {
                ++token_end;
                paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
                position = token_end;
            } else {
                ++position;
            }
        } else {
            ++position;
        }
    }
}

void paint_braced_parameters(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] != '{' || (position + 1 < end && context.source[position + 1] == '{')) {
            position += position + 1 < end && context.source[position] == '{' ? 2 : 1;
            continue;
        }
        const usize token_begin = position++;
        usize depth = 1;
        while (position < end && depth != 0) {
            if (context.source[position] == '{')
                ++depth;
            else if (context.source[position] == '}')
                --depth;
            ++position;
        }
        paint(context, {.begin = token_begin, .end = position}, Style::PARAMETER);
    }
}

struct QuotedToken {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] QuotedToken quoted_end(std::string_view source, usize position, usize end, char delimiter, bool escapes,
                                     bool doubled_quote = false) noexcept {
    while (position < end) {
        if (escapes && source[position] == '\\') {
            position = escape_end(source, position, end);
        } else if (source[position] == delimiter) {
            if (doubled_quote && position + 1 < end && source[position + 1] == delimiter) {
                position += 2;
            } else {
                return {.position = position + 1, .closed = true};
            }
        } else {
            ++position;
        }
    }
    return {.position = end};
}

[[nodiscard]] u32 lex_comment(LexContext &context, usize &position, usize end, u32 state, Dialect dialect) {
    u32 kind = state & k_state_mask;
    usize depth = std::max<u32>(1, state & k_state_payload_mask);
    const Style style = kind == k_doc_comment ? Style::DOCUMENTATION : Style::COMMENT;
    const usize token_begin = position;
    while (position < end) {
        if ((kind == k_nested_comment || nested_comments(dialect)) && context.source.substr(position).starts_with("/*")) {
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
    paint(context, {.begin = token_begin, .end = position}, style);
    kind = nested_comments(dialect) ? k_nested_comment : kind;
    return kind | static_cast<u32>(std::min<usize>(depth, k_state_payload_mask));
}

[[nodiscard]] u32 lex_multiline_string(LexContext &context, usize &position, usize end, u32 state) {
    const u32 kind = state & k_state_mask;
    const std::string_view delimiter = kind == k_triple_double ? "\"\"\"" : kind == k_triple_single ? "'''" : "`";
    const usize token_begin = position;
    const usize found = context.source.find(delimiter, position);
    position = found == std::string_view::npos || found >= end ? end : found + delimiter.size();
    paint(context, {.begin = token_begin, .end = position}, Style::STRING);
    if (kind != k_triple_single && kind != k_backtick_string) paint_string_details(context, token_begin, position);
    return found == std::string_view::npos || found >= end ? kind : k_normal;
}

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P');
}

[[nodiscard]] bool all_upper_identifier(std::string_view word) noexcept {
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

[[nodiscard]] Style identifier_style(std::string_view source, std::string_view word, usize begin, usize end, usize line_begin,
                                     usize line_end, Dialect dialect, PendingDeclaration &pending) noexcept {
    if (pending != PendingDeclaration::NONE) {
        const Style result = declaration_style(pending);
        pending = PendingDeclaration::NONE;
        return result;
    }
    if (k_constants.contains(word) || all_upper_identifier(word)) return Style::CONSTANT;
    if (builtin_type(word, dialect)) return Style::TYPE;
    if (keyword(word, dialect)) {
        pending = declaration_after(word);
        return Style::KEYWORD;
    }
    const usize previous = previous_non_space(source, begin, line_begin);
    const usize next = skip_space(source, end, line_end);
    if (next < line_end && source[next] == '(') return Style::FUNCTION;
    if (previous > line_begin && source[previous - 1] == '.') return Style::PROPERTY;
    if (next < line_end && source[next] == ':') return Style::LABEL;
    if (!word.empty() && ascii_upper(word.front())) return Style::TYPE;
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    PendingDeclaration pending = PendingDeclaration::NONE;
    while (position < line_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_block_comment || kind == k_doc_comment || kind == k_nested_comment) {
            state = lex_comment(context, position, line_end, state, dialect);
            if (state != k_normal) break;
            continue;
        }
        if (kind == k_triple_double || kind == k_triple_single || kind == k_backtick_string) {
            state = lex_multiline_string(context, position, line_end, state);
            if (state != k_normal) break;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        if (context.source.substr(position).starts_with("//") ||
            (context.source.substr(position).starts_with("#!") && position == skip_space(context.source, begin, line_end))) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if (context.source.substr(position).starts_with("/*")) {
            const bool documentation =
                context.source.substr(position).starts_with("/**") || context.source.substr(position).starts_with("/*!");
            state = (documentation ? k_doc_comment : nested_comments(dialect) ? k_nested_comment : k_block_comment) | 1;
            paint(context, {.begin = position, .end = position + 2}, documentation ? Style::DOCUMENTATION : Style::COMMENT);
            position += 2;
            continue;
        }
        if (position == skip_space(context.source, begin, line_end) && current == '#') {
            paint(context, {.begin = position, .end = line_end}, Style::PREPROCESSOR);
            break;
        }
        if (dialect == Dialect::ZIG && context.source.substr(position).starts_with("\\\\")) {
            paint(context, {.begin = position, .end = line_end}, Style::STRING);
            break;
        }
        if (triple_strings(dialect) &&
            (context.source.substr(position).starts_with("\"\"\"") || context.source.substr(position).starts_with("'''"))) {
            const bool double_quoted = current == '"';
            const u32 string_state = double_quoted ? k_triple_double : k_triple_single;
            const usize token_begin = position;
            position += 3;
            const usize found = context.source.find(double_quoted ? "\"\"\"" : "'''", position);
            position = found == std::string_view::npos || found >= line_end ? line_end : found + 3;
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            if (double_quoted) paint_string_details(context, token_begin, position);
            if (found == std::string_view::npos || found >= line_end) state = string_state;
            continue;
        }
        if (dialect == Dialect::D && current == '`') {
            const usize token_begin = position++;
            const usize found = context.source.find('`', position);
            position = found == std::string_view::npos || found >= line_end ? line_end : found + 1;
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            if (found == std::string_view::npos || found >= line_end) state = k_backtick_string;
            continue;
        }
        const bool csharp_interpolated = dialect == Dialect::CSHARP && (context.source.substr(position).starts_with("$\"") ||
                                                                        context.source.substr(position).starts_with("$@\"") ||
                                                                        context.source.substr(position).starts_with("@$\""));
        const bool verbatim = dialect == Dialect::CSHARP &&
                              (context.source.substr(position).starts_with("@\"") || context.source.substr(position).starts_with("$@\"") ||
                               context.source.substr(position).starts_with("@$\""));
        const bool raw_dart = dialect == Dialect::DART && (current == 'r' || current == 'R') && position + 1 < line_end &&
                              (context.source[position + 1] == '"' || context.source[position + 1] == '\'');
        if (current == '"' || current == '\'' || verbatim || raw_dart || csharp_interpolated) {
            const usize token_begin = position;
            if (context.source.substr(position).starts_with("$@\"") || context.source.substr(position).starts_with("@$\""))
                position += 2;
            else if (verbatim || raw_dart || csharp_interpolated)
                ++position;
            const char quote = context.source[position++];
            const QuotedToken token = quoted_end(context.source, position, line_end, quote, !verbatim && !raw_dart, verbatim);
            position = token.position;
            const Style style = quote == '\'' && dialect != Dialect::DART && dialect != Dialect::GROOVY && dialect != Dialect::GRADLE &&
                                        dialect != Dialect::KOTLIN && dialect != Dialect::SCALA ?
                                    Style::CHARACTER :
                                    Style::STRING;
            paint(context, {.begin = token_begin, .end = position}, style);
            if (!raw_dart) paint_string_details(context, token_begin, position);
            if (csharp_interpolated) paint_braced_parameters(context, token_begin, position);
            continue;
        }
        if (dialect == Dialect::CSHARP && current == '[') {
            const usize close = context.source.find(']', position + 1);
            if (close != std::string_view::npos && close < line_end) {
                paint(context, {.begin = position, .end = close + 1}, Style::ATTRIBUTE);
                position = close + 1;
                continue;
            }
        }
        if (current == '@' && position + 1 < line_end && ascii_identifier_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && (ascii_identifier_continue(context.source[position]) || context.source[position] == '.'))
                ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::ATTRIBUTE);
            continue;
        }
        if (ascii_digit(current) || (current == '.' && position + 1 < line_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < line_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            continue;
        }
        if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < line_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            paint(context, {.begin = token_begin, .end = position},
                  identifier_style(context.source, word, token_begin, position, begin, line_end, dialect, pending));
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

    if (end > line_end && (state & k_state_mask) != k_normal) {
        const u32 kind = state & k_state_mask;
        paint(context, {.begin = line_end, .end = end},
              kind == k_block_comment || kind == k_nested_comment ? Style::COMMENT :
              kind == k_doc_comment                               ? Style::DOCUMENTATION :
                                                                    Style::STRING);
    }
    return state;
}

} // namespace

void BraceLexer::lex(LexContext &context) const {
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
