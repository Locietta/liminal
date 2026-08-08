#include "legacy.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexAPDL.cxx, LexFortran.cxx, LexPascal.cxx,
// LexPowerBuilder.cxx, LexSAS.cxx, LexVB.cxx, and matching stl*.cpp data at
// revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. APDL/ABAQUS and VB/VBScript
// remain distinct configured identities over their shared upstream lexer
// implementations. This port retains declarations, directives, macro
// variables, types, case-insensitive keywords, and multiline comments.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = LegacyDialect;
using Style = LegacyLexer::Style;

constexpr auto k_apdl = make_word_set("allsel", "antype", "cmsel", "d", "e", "esel", "finish", "k", "l", "lesize", "mat", "mp", "n", "nsel",
                                      "prep7", "solve", "type");
constexpr auto k_fortran = make_word_set(
    "abstract", "allocatable", "allocate", "associate", "asynchronous", "backspace", "bind", "block", "call", "case", "class", "close",
    "common", "contains", "continue", "critical", "cycle", "data", "deallocate", "default", "deferred", "dimension", "do", "else", "elseif",
    "elsewhere", "end", "enddo", "endif", "entry", "enum", "equivalence", "error", "exit", "extends", "external", "final", "flush",
    "forall", "format", "function", "generic", "goto", "if", "implicit", "import", "inquire", "intent", "interface", "intrinsic", "module",
    "namelist", "non_overridable", "nopass", "nullify", "only", "open", "operator", "optional", "parameter", "pass", "pause", "pointer",
    "print", "private", "procedure", "program", "protected", "public", "pure", "read", "recursive", "result", "return", "rewind", "save",
    "select", "sequence", "stop", "subroutine", "target", "then", "use", "value", "volatile", "wait", "where", "while", "write");
constexpr auto k_fortran_types = make_word_set("character", "complex", "double", "integer", "logical", "real", "type");
constexpr auto k_pascal = make_word_set(
    "absolute", "and", "array", "as", "asm", "begin", "case", "class", "const", "constructor", "destructor", "dispinterface", "div", "do",
    "downto", "else", "end", "except", "exports", "file", "finalization", "finally", "for", "function", "goto", "if", "implementation",
    "in", "inherited", "initialization", "inline", "interface", "is", "label", "library", "mod", "nil", "not", "object", "of", "operator",
    "or", "out", "packed", "procedure", "program", "property", "raise", "record", "repeat", "resourcestring", "set", "shl", "shr", "string",
    "then", "threadvar", "to", "try", "type", "unit", "until", "uses", "var", "while", "with", "xor");
constexpr auto k_pascal_types =
    make_word_set("ansichar", "ansistring", "boolean", "byte", "cardinal", "char", "currency", "double", "extended", "int64", "integer",
                  "longint", "pointer", "real", "shortint", "single", "smallint", "string", "variant", "widechar", "widestring", "word");
constexpr auto k_powerbuilder = make_word_set(
    "alias", "and", "autoinstantiate", "call", "case", "catch", "choose", "close", "commit", "connect", "constant", "continue", "create",
    "cursor", "declare", "delete", "describe", "disconnect", "do", "dynamic", "else", "elseif", "end", "enumerated", "event", "execute",
    "exit", "external", "finally", "for", "forward", "from", "function", "global", "goto", "if", "immediate", "indirect", "insert", "into",
    "is", "local", "loop", "namespace", "next", "not", "of", "on", "open", "or", "parent", "post", "prepare", "private", "procedure",
    "protected", "public", "readonly", "ref", "return", "rollback", "rpcfunc", "select", "shared", "static", "subroutine", "super",
    "system", "then", "throw", "to", "trigger", "try", "type", "update", "using", "values", "variables", "while", "with", "within");
constexpr auto k_sas =
    make_word_set("array", "attrib", "by", "call", "cards", "class", "data", "delete", "do", "drop", "else", "end", "error", "file",
                  "first", "format", "if", "informat", "input", "keep", "label", "length", "libname", "link", "lostcard", "merge",
                  "missing", "modify", "options", "otherwise", "output", "proc", "quit", "remove", "rename", "replace", "retain", "return",
                  "run", "select", "set", "stop", "then", "title", "update", "where", "window");
constexpr auto k_vb = make_word_set(
    "addhandler", "addressof", "alias", "and", "andalso", "as", "boolean", "byref", "byte", "byval", "call", "case", "catch", "class",
    "const", "continue", "date", "decimal", "declare", "default", "delegate", "dim", "directcast", "do", "double", "each", "else", "elseif",
    "end", "enum", "erase", "error", "event", "exit", "false", "finally", "for", "friend", "function", "get", "gettype", "global", "gosub",
    "goto", "handles", "if", "implements", "imports", "in", "inherits", "integer", "interface", "is", "isnot", "let", "lib", "like", "long",
    "loop", "me", "mod", "module", "mustinherit", "mustoverride", "mybase", "myclass", "namespace", "narrowing", "new", "next", "not",
    "nothing", "notinheritable", "notoverridable", "object", "of", "on", "operator", "option", "optional", "or", "orelse", "overloads",
    "overridable", "overrides", "paramarray", "partial", "private", "property", "protected", "public", "raiseevent", "readonly", "redim",
    "rem", "removehandler", "resume", "return", "sbyte", "select", "set", "shadows", "shared", "short", "single", "static", "step", "stop",
    "string", "structure", "sub", "synclock", "then", "throw", "to", "true", "try", "typeof", "uinteger", "ulong", "ushort", "using",
    "variant", "wend", "when", "while", "widening", "with", "withevents", "writeonly", "xor");
constexpr auto k_constants = make_word_set("False", "Nothing", "True", "false", "nil", "null", "true");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_c_comment = 0x1000'0000;
constexpr u32 k_pascal_brace = 0x2000'0000;
constexpr u32 k_pascal_paren = 0x3000'0000;

enum struct Pending : u8 {
    NONE,
    FUNCTION,
    TYPE,
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

[[nodiscard]] bool equal_ascii_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index])) return false;
    }
    return true;
}

template <usize Size>
[[nodiscard]] bool contains_ascii_ci(const WordSet<Size> &set, std::string_view word) noexcept {
    return std::ranges::any_of(set.words, [&](std::string_view candidate) { return equal_ascii_ci(candidate, word); });
}

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '$' || value == '#';
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\#").contains(value); }

[[nodiscard]] bool keyword(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::APDL:
        case Dialect::ABAQUS: return contains_ascii_ci(k_apdl, word);
        case Dialect::FORTRAN: return contains_ascii_ci(k_fortran, word);
        case Dialect::PASCAL: return contains_ascii_ci(k_pascal, word);
        case Dialect::POWERBUILDER: return contains_ascii_ci(k_powerbuilder, word);
        case Dialect::SAS: return contains_ascii_ci(k_sas, word);
        case Dialect::VISUAL_BASIC:
        case Dialect::VBSCRIPT: return contains_ascii_ci(k_vb, word);
    }
    return false;
}

[[nodiscard]] bool builtin_type(std::string_view word, Dialect dialect) noexcept {
    if (dialect == Dialect::FORTRAN) return contains_ascii_ci(k_fortran_types, word);
    if (dialect == Dialect::PASCAL) return contains_ascii_ci(k_pascal_types, word);
    if (dialect == Dialect::VISUAL_BASIC || dialect == Dialect::VBSCRIPT) {
        return equal_ascii_ci(word, "Boolean") || equal_ascii_ci(word, "Byte") || equal_ascii_ci(word, "Date") ||
               equal_ascii_ci(word, "Decimal") || equal_ascii_ci(word, "Double") || equal_ascii_ci(word, "Integer") ||
               equal_ascii_ci(word, "Long") || equal_ascii_ci(word, "Object") || equal_ascii_ci(word, "Short") ||
               equal_ascii_ci(word, "Single") || equal_ascii_ci(word, "String") || equal_ascii_ci(word, "Variant");
    }
    return false;
}

[[nodiscard]] Pending declaration_after(std::string_view word) noexcept {
    if (equal_ascii_ci(word, "class") || equal_ascii_ci(word, "enum") || equal_ascii_ci(word, "interface") ||
        equal_ascii_ci(word, "structure") || equal_ascii_ci(word, "type"))
        return Pending::TYPE;
    if (equal_ascii_ci(word, "function") || equal_ascii_ci(word, "procedure") || equal_ascii_ci(word, "program") ||
        equal_ascii_ci(word, "sub") || equal_ascii_ci(word, "subroutine"))
        return Pending::FUNCTION;
    if (equal_ascii_ci(word, "library") || equal_ascii_ci(word, "module") || equal_ascii_ci(word, "namespace") ||
        equal_ascii_ci(word, "unit"))
        return Pending::MODULE;
    if (equal_ascii_ci(word, "const") || equal_ascii_ci(word, "dim") || equal_ascii_ci(word, "parameter") ||
        equal_ascii_ci(word, "property") || equal_ascii_ci(word, "signal") || equal_ascii_ci(word, "var") ||
        equal_ascii_ci(word, "variable"))
        return Pending::PROPERTY;
    return Pending::NONE;
}

[[nodiscard]] Style pending_style(Pending pending) noexcept {
    switch (pending) {
        case Pending::FUNCTION: return Style::FUNCTION;
        case Pending::TYPE: return Style::TYPE;
        case Pending::MODULE: return Style::MODULE;
        case Pending::PROPERTY: return Style::PROPERTY;
        case Pending::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote, Dialect dialect) noexcept {
    while (position < end) {
        if (source[position] == quote) {
            if ((dialect == Dialect::VISUAL_BASIC || dialect == Dialect::VBSCRIPT || dialect == Dialect::PASCAL) && position + 1 < end &&
                source[position + 1] == quote) {
                position += 2;
                continue;
            }
            return position + 1;
        }
        if (source[position] == '\\' && position + 1 < end && dialect != Dialect::VISUAL_BASIC && dialect != Dialect::VBSCRIPT)
            position += 2;
        else
            ++position;
    }
    return end;
}

void paint_escapes(LexContext &context, usize begin, usize end, Dialect dialect) {
    for (usize position = begin; position < end;) {
        if ((context.source[position] == '\\' || (dialect == Dialect::POWERBUILDER && context.source[position] == '~')) &&
            position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] u32 continue_comment(LexContext &context, usize &position, usize line_end, u32 state) {
    const u32 kind = state & k_state_mask;
    const std::string_view close = kind == k_pascal_brace ? "}" : kind == k_pascal_paren ? "*)" : "*/";
    const usize found = context.source.find(close, position);
    const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + close.size();
    paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
    position = token_end;
    return found == std::string_view::npos || found >= line_end ? state : k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    if ((state & k_state_mask) != k_normal) {
        state = continue_comment(context, position, line_end, state);
        if (state != k_normal) {
            if (end > line_end) paint(context, {.begin = line_end, .end = end}, Style::COMMENT);
            return state;
        }
    }
    const usize first = skip_space(context.source, position, line_end);
    if (first >= line_end) return k_normal;
    if (dialect == Dialect::ABAQUS && context.source.substr(first, 2) == "**") {
        paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
        return k_normal;
    }
    if (dialect == Dialect::FORTRAN && first == begin &&
        (context.source[first] == '*' || ((context.source[first] == 'c' || context.source[first] == 'C') &&
                                          (first + 1 == line_end || ascii_space(context.source[first + 1]))))) {
        paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
        return k_normal;
    }
    if ((dialect == Dialect::VISUAL_BASIC || dialect == Dialect::VBSCRIPT) && context.source.substr(first).size() >= 3 &&
        equal_ascii_ci(context.source.substr(first, 3), "rem") && (first + 3 == line_end || ascii_space(context.source[first + 3]))) {
        paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
        return k_normal;
    }
    if (dialect == Dialect::SAS && context.source[first] == '*') {
        const usize semicolon = context.source.find(';', first + 1);
        const usize token_end = semicolon == std::string_view::npos || semicolon >= line_end ? line_end : semicolon + 1;
        paint(context, {.begin = first, .end = token_end}, Style::COMMENT);
        position = token_end;
    }

    Pending pending = Pending::NONE;
    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        if ((dialect == Dialect::APDL && current == '!') ||
            ((dialect == Dialect::VISUAL_BASIC || dialect == Dialect::VBSCRIPT) && current == '\'') ||
            (dialect == Dialect::FORTRAN && current == '!') ||
            (dialect == Dialect::POWERBUILDER && context.source.substr(position, 2) == "//")) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if ((dialect == Dialect::POWERBUILDER || dialect == Dialect::SAS) && context.source.substr(position, 2) == "/*") {
            const usize found = context.source.find("*/", position + 2);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) return k_c_comment;
            continue;
        }
        if (dialect == Dialect::PASCAL && (current == '{' || context.source.substr(position, 2) == "(*")) {
            const bool brace = current == '{';
            const std::string_view close = brace ? "}" : "*)";
            const usize found = context.source.find(close, position + (brace ? 1 : 2));
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + close.size();
            const bool directive = brace && position + 1 < line_end && context.source[position + 1] == '$';
            paint(context, {.begin = position, .end = token_end}, directive ? Style::DIRECTIVE : Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) return brace ? k_pascal_brace : k_pascal_paren;
            continue;
        }
        if ((dialect == Dialect::APDL || dialect == Dialect::ABAQUS) && current == '*' && position == first) {
            const usize token_end = context.source.find_first_of(", \t", position);
            paint(context, {.begin = position, .end = token_end == std::string_view::npos || token_end > line_end ? line_end : token_end},
                  Style::DIRECTIVE);
            position = token_end == std::string_view::npos || token_end > line_end ? line_end : token_end;
            continue;
        }
        if ((dialect == Dialect::VISUAL_BASIC || dialect == Dialect::VBSCRIPT || dialect == Dialect::FORTRAN) && current == '#' &&
            position == first) {
            paint(context, {.begin = position, .end = line_end}, Style::DIRECTIVE);
            break;
        }
        if (dialect == Dialect::SAS && (current == '%' || current == '&')) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, current == '%' ? Style::DIRECTIVE : Style::VARIABLE);
            continue;
        }
        if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current, dialect);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_escapes(context, token_begin, position, dialect);
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
            if (pending != Pending::NONE) {
                style = pending_style(pending);
                pending = Pending::NONE;
            } else if (contains_ascii_ci(k_constants, word)) {
                style = Style::CONSTANT;
            } else if (builtin_type(word, dialect)) {
                style = Style::TYPE;
            } else if (keyword(word, dialect)) {
                style = Style::KEYWORD;
                pending = declaration_after(word);
            } else if (next < line_end && context.source[next] == '(') {
                style = Style::FUNCTION;
            } else if (previous > begin && context.source[previous - 1] == '.') {
                style = Style::PROPERTY;
            } else if ((dialect == Dialect::APDL || dialect == Dialect::ABAQUS) && token_begin == first) {
                style = Style::FUNCTION;
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

void LegacyLexer::lex(LexContext &context) const {
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
