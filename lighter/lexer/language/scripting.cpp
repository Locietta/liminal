#include "scripting.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexAutoHotkey.cxx, LexAutoIt3.cxx,
// LexAviSynth.cxx, LexAwk.cxx, LexCoffeeScript.cxx, LexJulia.cxx, LexLua.cxx,
// LexMathematica.cxx, LexMatlab.cxx, LexNim.cxx, LexPerl.cxx, LexPHP.cxx,
// LexPowerShell.cxx, LexR.cxx, LexRebol.cxx, LexRuby.cxx, LexTCL.cxx,
// LexVim.cxx, and matching stl*.cpp data at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The port retains distinct
// language identities, declarations, sigils, interpolation, nested comments,
// long-bracket strings, here-strings, POD-style comments, and triple strings.
// Editor folding and runtime keyword properties are omitted.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = ScriptingDialect;
using Style = ScriptingLexer::Style;

constexpr auto k_autohotkey = make_word_set("break", "case", "catch", "class", "continue", "else", "finally", "for", "global", "if",
                                            "local", "loop", "return", "static", "switch", "throw", "try", "until", "while");
constexpr auto k_autoit =
    make_word_set("and", "byref", "case", "const", "continuecase", "continueloop", "default", "dim", "do", "else", "elseif", "endfunc",
                  "endif", "endselect", "endswitch", "endwhile", "enum", "exitloop", "false", "for", "func", "global", "if", "in", "local",
                  "next", "not", "null", "or", "return", "select", "step", "switch", "then", "true", "until", "volatile", "while", "with");
constexpr auto k_avisynth =
    make_word_set("bool", "catch", "clip", "else", "float", "function", "global", "if", "int", "last", "return", "string", "try", "val");
constexpr auto k_awk = make_word_set("BEGIN", "END", "break", "continue", "delete", "do", "else", "exit", "for", "function", "getline",
                                     "if", "in", "next", "nextfile", "print", "printf", "return", "while");
constexpr auto k_coffee =
    make_word_set("and", "break", "by", "catch", "class", "continue", "delete", "do", "else", "extends", "false", "finally", "for", "if",
                  "in", "instanceof", "is", "isnt", "loop", "new", "no", "not", "null", "of", "off", "on", "or", "return", "super",
                  "switch", "then", "this", "throw", "true", "try", "typeof", "unless", "until", "when", "while", "yes");
constexpr auto k_julia = make_word_set("abstract", "baremodule", "begin", "break", "catch", "const", "continue", "do", "else", "elseif",
                                       "end", "export", "finally", "for", "function", "global", "if", "import", "let", "local", "macro",
                                       "module", "mutable", "primitive", "quote", "return", "struct", "try", "using", "where", "while");
constexpr auto k_lua = make_word_set("and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local",
                                     "nil", "not", "or", "repeat", "return", "then", "true", "until", "while");
constexpr auto k_mathematica = make_word_set("Block", "Catch", "Compile", "Do", "For", "Function", "If", "Module", "Nest", "Return",
                                             "Switch", "Table", "Throw", "Which", "While", "With");
constexpr auto k_matlab =
    make_word_set("break", "case", "catch", "classdef", "continue", "else", "elseif", "end", "enumeration", "events", "for", "function",
                  "global", "if", "methods", "otherwise", "parfor", "persistent", "properties", "return", "spmd", "switch", "try", "while");
constexpr auto k_nim =
    make_word_set("addr", "and", "as", "asm", "bind", "block", "break", "case", "cast", "concept", "const", "continue", "converter",
                  "defer", "discard", "distinct", "div", "do", "elif", "else", "end", "enum", "except", "export", "finally", "for", "from",
                  "func", "generic", "if", "import", "in", "include", "interface", "is", "iterator", "let", "macro", "method", "mixin",
                  "mod", "nil", "not", "object", "of", "or", "out", "proc", "ptr", "raise", "ref", "return", "shl", "shr", "static",
                  "template", "try", "tuple", "type", "using", "var", "when", "while", "with", "without", "xor", "yield");
constexpr auto k_perl =
    make_word_set("continue", "do", "else", "elsif", "eval", "for", "foreach", "format", "given", "goto", "if", "last", "local", "my",
                  "next", "no", "our", "package", "redo", "require", "return", "state", "sub", "unless", "until", "use", "when", "while");
constexpr auto k_php =
    make_word_set("abstract", "and", "array", "as", "break", "callable", "case", "catch", "class", "clone", "const", "continue", "declare",
                  "default", "do", "echo", "else", "elseif", "empty", "enddeclare", "endfor", "endforeach", "endif", "endswitch",
                  "endwhile", "enum", "eval", "exit", "extends", "final", "finally", "fn", "for", "foreach", "function", "global", "goto",
                  "if", "implements", "include", "include_once", "instanceof", "insteadof", "interface", "isset", "list", "match",
                  "namespace", "new", "or", "print", "private", "protected", "public", "readonly", "require", "require_once", "return",
                  "static", "switch", "throw", "trait", "try", "unset", "use", "var", "while", "xor", "yield");
constexpr auto k_powershell =
    make_word_set("begin", "break", "catch", "class", "continue", "data", "define", "do", "dynamicparam", "else", "elseif", "end", "enum",
                  "exit", "filter", "finally", "for", "foreach", "from", "function", "if", "in", "param", "process", "return", "switch",
                  "throw", "trap", "try", "until", "using", "while", "workflow");
constexpr auto k_r = make_word_set("break", "else", "for", "function", "if", "in", "next", "repeat", "return", "while");
constexpr auto k_rebol =
    make_word_set("any", "break", "catch", "compose", "do", "either", "else", "exit", "forall", "foreach", "forever", "forskip", "func",
                  "function", "if", "loop", "reduce", "repeat", "return", "switch", "throw", "until", "while");
constexpr auto k_ruby =
    make_word_set("BEGIN", "END", "alias", "and", "begin", "break", "case", "class", "def", "defined", "do", "else", "elsif", "end",
                  "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo", "rescue", "retry", "return", "self",
                  "super", "then", "true", "undef", "unless", "until", "when", "while", "yield");
constexpr auto k_tcl = make_word_set(
    "after", "append", "apply", "array", "break", "catch", "concat", "continue", "dict", "else", "elseif", "error", "eval", "expr", "for",
    "foreach", "format", "global", "if", "incr", "info", "lappend", "lassign", "lindex", "linsert", "list", "llength", "lmap", "load",
    "lrange", "lreplace", "lreverse", "lsearch", "lset", "lsort", "namespace", "proc", "puts", "regexp", "regsub", "rename", "return",
    "set", "source", "string", "subst", "switch", "trace", "unset", "update", "uplevel", "upvar", "variable", "while");
constexpr auto k_vim = make_word_set("autocmd", "break", "call", "catch", "command", "continue", "def", "else", "elseif", "enddef",
                                     "endfor", "endfunction", "endif", "endtry", "endwhile", "execute", "finish", "for", "function", "if",
                                     "import", "let", "lockvar", "return", "set", "source", "throw", "try", "unlet", "var", "while");
constexpr auto k_types = make_word_set("Any", "Array", "Bool", "Boolean", "Dict", "Float", "Int", "Integer", "List", "Number", "Object",
                                       "String", "Symbol", "Tuple", "Vector", "bool", "boolean", "byte", "char", "double", "float", "int",
                                       "integer", "long", "number", "short", "string", "void");
constexpr auto k_constants = make_word_set("False", "Inf", "NA", "NULL", "NaN", "None", "True", "false", "nil", "null", "true");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_long_string = 0x2000'0000;
constexpr u32 k_triple_double = 0x3000'0000;
constexpr u32 k_triple_single = 0x4000'0000;
constexpr u32 k_here_double = 0x5000'0000;
constexpr u32 k_here_single = 0x6000'0000;
constexpr u32 k_long_comment = 0x7000'0000;

enum struct Pending : u8 {
    NONE,
    FUNCTION,
    TYPE,
    MODULE,
    PROPERTY,
};

struct Delimiters {
    std::string_view open;
    std::string_view close;
    bool nested = false;
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

[[nodiscard]] bool insensitive(Dialect dialect) noexcept {
    return dialect == Dialect::AUTOHOTKEY || dialect == Dialect::AUTOIT || dialect == Dialect::AVISYNTH || dialect == Dialect::POWERSHELL ||
           dialect == Dialect::REBOL || dialect == Dialect::VIM;
}

template <usize Size>
[[nodiscard]] bool contains(const WordSet<Size> &set, std::string_view word, Dialect dialect) noexcept {
    return insensitive(dialect) ? contains_ascii_ci(set, word) : set.contains(word);
}

[[nodiscard]] bool keyword(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::AUTOHOTKEY: return contains(k_autohotkey, word, dialect);
        case Dialect::AUTOIT: return contains(k_autoit, word, dialect);
        case Dialect::AVISYNTH: return contains(k_avisynth, word, dialect);
        case Dialect::AWK: return k_awk.contains(word);
        case Dialect::COFFEESCRIPT: return k_coffee.contains(word);
        case Dialect::JULIA: return k_julia.contains(word);
        case Dialect::LUA: return k_lua.contains(word);
        case Dialect::MATHEMATICA: return k_mathematica.contains(word);
        case Dialect::MATLAB: return k_matlab.contains(word);
        case Dialect::NIM: return k_nim.contains(word);
        case Dialect::PERL: return k_perl.contains(word);
        case Dialect::PHP: return k_php.contains(word);
        case Dialect::POWERSHELL: return contains(k_powershell, word, dialect);
        case Dialect::R: return k_r.contains(word);
        case Dialect::REBOL: return contains(k_rebol, word, dialect);
        case Dialect::RUBY: return k_ruby.contains(word);
        case Dialect::TCL: return k_tcl.contains(word);
        case Dialect::VIM: return contains(k_vim, word, dialect);
    }
    return false;
}

[[nodiscard]] Delimiters comment_delimiters(Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::AUTOHOTKEY:
        case Dialect::AVISYNTH:
        case Dialect::PHP: return {.open = "/*", .close = "*/"};
        case Dialect::COFFEESCRIPT: return {.open = "###", .close = "###"};
        case Dialect::JULIA: return {.open = "#=", .close = "=#", .nested = true};
        case Dialect::MATHEMATICA: return {.open = "(*", .close = "*)", .nested = true};
        case Dialect::MATLAB: return {.open = "%{", .close = "%}"};
        case Dialect::NIM: return {.open = "#[", .close = "]#", .nested = true};
        case Dialect::POWERSHELL: return {.open = "<#", .close = "#>"};
        default: return {};
    }
}

[[nodiscard]] bool line_comment(std::string_view source, usize position, usize first, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::AUTOHOTKEY:
        case Dialect::AUTOIT:
        case Dialect::REBOL: return source[position] == ';';
        case Dialect::AVISYNTH:
        case Dialect::PHP: return source.substr(position, 2) == "//" || source[position] == '#';
        case Dialect::AWK:
        case Dialect::COFFEESCRIPT:
        case Dialect::JULIA:
        case Dialect::NIM:
        case Dialect::PERL:
        case Dialect::POWERSHELL:
        case Dialect::R:
        case Dialect::RUBY:
        case Dialect::TCL: return source[position] == '#';
        case Dialect::LUA: return source.substr(position, 2) == "--";
        case Dialect::MATHEMATICA: return false;
        case Dialect::MATLAB: return source[position] == '%';
        case Dialect::VIM: return source[position] == '"' && position == first;
    }
    return false;
}

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '?' || value == '!' || value == '\'';
}

[[nodiscard]] bool powershell_word_continue(char value) noexcept { return identifier_continue(value) || value == '-'; }

[[nodiscard]] bool powershell_command_after(std::string_view word) noexcept {
    return equal_ascii_ci(word, "in") || equal_ascii_ci(word, "return");
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\#").contains(value); }

struct LongDelimiter {
    usize length = 0;
    usize equals = 0;
};

[[nodiscard]] LongDelimiter long_opener(std::string_view source, usize position, usize end) noexcept {
    if (position >= end || source[position] != '[') return {};
    usize cursor = position + 1;
    while (cursor < end && source[cursor] == '=') ++cursor;
    if (cursor >= end || source[cursor] != '[') return {};
    return {.length = cursor - position + 1, .equals = cursor - position - 1};
}

[[nodiscard]] usize long_close(std::string_view source, usize position, usize end, usize equals) noexcept {
    while (position < end) {
        const usize close = source.find(']', position);
        if (close == std::string_view::npos || close >= end) return end;
        usize cursor = close + 1;
        usize count = 0;
        while (cursor < end && source[cursor] == '=' && count < equals) {
            ++cursor;
            ++count;
        }
        if (count == equals && cursor < end && source[cursor] == ']') return cursor + 1;
        position = close + 1;
    }
    return end;
}

[[nodiscard]] bool long_closed_at(std::string_view source, usize end, usize equals) noexcept {
    if (end < equals + 2 || source[end - 1] != ']') return false;
    usize position = end - 2;
    for (usize count = 0; count < equals; ++count) {
        if (source[position] != '=') return false;
        --position;
    }
    return source[position] == ']';
}

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote) noexcept {
    while (position < end) {
        if (source[position] == '\\' && position + 1 < end)
            position += 2;
        else if (source[position++] == quote)
            return position;
    }
    return end;
}

void paint_string_details(LexContext &context, usize begin, usize end, Dialect dialect) {
    for (usize position = begin; position < end;) {
        const char current = context.source[position];
        if (current == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else if (current == '$' && dialect != Dialect::MATLAB && dialect != Dialect::MATHEMATICA) {
            const usize token_begin = position++;
            if (position < end && context.source[position] == '{') {
                ++position;
                while (position < end && context.source[position] != '}') ++position;
                if (position < end) ++position;
            } else {
                while (position < end && identifier_continue(context.source[position])) ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::PARAMETER);
        } else {
            ++position;
        }
    }
}

[[nodiscard]] Pending declaration_after(std::string_view word, Dialect dialect) noexcept {
    if (word == "class" || word == "classdef" || word == "concept" || word == "enum" || word == "struct" || word == "trait" ||
        word == "type")
        return Pending::TYPE;
    if (word == "def" || word == "filter" || word == "func" || word == "function" || word == "macro" || word == "method" ||
        word == "proc" || word == "sub")
        return Pending::FUNCTION;
    if (word == "import" || word == "module" || word == "namespace" || word == "package" || word == "use" || word == "using")
        return Pending::MODULE;
    if ((dialect == Dialect::JULIA || dialect == Dialect::NIM) && (word == "const" || word == "let" || word == "var"))
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

[[nodiscard]] bool line_block_open(std::string_view source, usize first, Dialect dialect) noexcept {
    const std::string_view rest = source.substr(first);
    return (dialect == Dialect::AUTOIT && (rest.starts_with("#cs") || rest.starts_with("#comments-start"))) ||
           (dialect == Dialect::PERL && (rest.starts_with("=pod") || rest.starts_with("=begin"))) ||
           (dialect == Dialect::RUBY && rest.starts_with("=begin"));
}

[[nodiscard]] bool line_block_close(std::string_view source, usize first, Dialect dialect) noexcept {
    const std::string_view rest = source.substr(first);
    return (dialect == Dialect::AUTOIT && (rest.starts_with("#ce") || rest.starts_with("#comments-end"))) ||
           (dialect == Dialect::PERL && rest.starts_with("=cut")) || (dialect == Dialect::RUBY && rest.starts_with("=end"));
}

[[nodiscard]] u32 continue_block(LexContext &context, usize &position, usize line_end, u32 state, Dialect dialect) {
    const usize first = skip_space(context.source, position, line_end);
    if (dialect == Dialect::AUTOIT || dialect == Dialect::PERL || dialect == Dialect::RUBY) {
        paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
        position = line_end;
        return line_block_close(context.source, first, dialect) ? k_normal : state;
    }
    const Delimiters delimiters = comment_delimiters(dialect);
    u32 depth = std::max<u32>(1, state & k_payload_mask);
    const usize token_begin = position;
    if (delimiters.open == delimiters.close) {
        const usize close = context.source.find(delimiters.close, position);
        position = close == std::string_view::npos || close >= line_end ? line_end : close + delimiters.close.size();
        paint(context, {.begin = token_begin, .end = position}, Style::COMMENT);
        return close == std::string_view::npos || close >= line_end ? state : k_normal;
    }
    while (position < line_end && depth != 0) {
        if (delimiters.nested && context.source.substr(position).starts_with(delimiters.open)) {
            ++depth;
            position += delimiters.open.size();
        } else if (context.source.substr(position).starts_with(delimiters.close)) {
            --depth;
            position += delimiters.close.size();
        } else {
            ++position;
        }
    }
    paint(context, {.begin = token_begin, .end = position}, Style::COMMENT);
    return depth == 0 ? k_normal : k_block_comment | depth;
}

[[nodiscard]] u32 continue_string(LexContext &context, usize &position, usize line_end, u32 state, Dialect dialect) {
    const u32 kind = state & k_state_mask;
    std::string_view delimiter;
    if (kind == k_triple_double)
        delimiter = "\"\"\"";
    else if (kind == k_triple_single)
        delimiter = "'''";
    else if (kind == k_here_double)
        delimiter = "\"@";
    else
        delimiter = "'@";
    const usize token_begin = position;
    const usize first = skip_space(context.source, position, line_end);
    const usize close = (kind == k_here_double || kind == k_here_single) && context.source.substr(first).starts_with(delimiter) ?
                            first :
                            context.source.find(delimiter, position);
    const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + delimiter.size();
    paint(context, {.begin = token_begin, .end = token_end}, Style::STRING);
    paint_string_details(context, token_begin, token_end, dialect);
    position = token_end;
    return close == std::string_view::npos || close >= line_end ? state : k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    const u32 kind = state & k_state_mask;
    if (kind == k_block_comment)
        state = continue_block(context, position, line_end, state, dialect);
    else if (kind == k_long_string || kind == k_long_comment) {
        const usize equals = (state & k_payload_mask) - 1;
        const usize token_end = long_close(context.source, position, line_end, equals);
        paint(context, {.begin = position, .end = token_end}, kind == k_long_comment ? Style::COMMENT : Style::STRING);
        position = token_end;
        state = token_end == line_end && !long_closed_at(context.source, token_end, equals) ? state : k_normal;
    } else if (kind != k_normal) {
        state = continue_string(context, position, line_end, state, dialect);
    }
    if (state != k_normal) {
        if (end > line_end)
            paint(context, {.begin = line_end, .end = end},
                  kind == k_block_comment || kind == k_long_comment ? Style::COMMENT : Style::STRING);
        return state;
    }

    const usize first = skip_space(context.source, position, line_end);
    if (line_block_open(context.source, first, dialect)) {
        paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
        return k_block_comment | 1;
    }
    Pending pending = Pending::NONE;
    bool command_position = dialect == Dialect::POWERSHELL;
    bool command_arguments = false;
    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        if (dialect == Dialect::LUA && context.source.substr(position, 2) == "--") {
            const LongDelimiter delimiter = long_opener(context.source, position + 2, line_end);
            if (delimiter.length != 0) {
                const usize token_end = long_close(context.source, position + 2 + delimiter.length, line_end, delimiter.equals);
                paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
                position = token_end;
                if (token_end == line_end && !long_closed_at(context.source, token_end, delimiter.equals))
                    return k_long_comment | static_cast<u32>(delimiter.equals + 1);
                continue;
            }
        }
        const Delimiters delimiters = comment_delimiters(dialect);
        if (!delimiters.open.empty() && context.source.substr(position).starts_with(delimiters.open)) {
            const usize token_begin = position;
            position += delimiters.open.size();
            state = continue_block(context, position, line_end, k_block_comment | 1, dialect);
            paint(context, {.begin = token_begin, .end = token_begin + delimiters.open.size()}, Style::COMMENT);
            if (state != k_normal) return state;
            continue;
        }
        if (line_comment(context.source, position, first, dialect)) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if (dialect == Dialect::LUA && current == '[') {
            const LongDelimiter delimiter = long_opener(context.source, position, line_end);
            if (delimiter.length != 0) {
                const usize token_begin = position;
                const usize token_end = long_close(context.source, position + delimiter.length, line_end, delimiter.equals);
                paint(context, {.begin = token_begin, .end = token_end}, Style::STRING);
                position = token_end;
                if (token_end == line_end && !long_closed_at(context.source, token_end, delimiter.equals))
                    return k_long_string | static_cast<u32>(delimiter.equals + 1);
                continue;
            }
        }
        if ((dialect == Dialect::JULIA || dialect == Dialect::NIM || dialect == Dialect::COFFEESCRIPT) &&
            (context.source.substr(position).starts_with("\"\"\"") || context.source.substr(position).starts_with("'''"))) {
            const usize token_begin = position;
            const bool double_quote = current == '"';
            position += 3;
            state = continue_string(context, position, line_end, double_quote ? k_triple_double : k_triple_single, dialect);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            if (state != k_normal) return state;
            continue;
        }
        if (dialect == Dialect::POWERSHELL &&
            (context.source.substr(position).starts_with("@\"") || context.source.substr(position).starts_with("@'"))) {
            const usize token_begin = position;
            const bool double_quote = context.source[position + 1] == '"';
            position += 2;
            paint(context, {.begin = token_begin, .end = line_end}, Style::STRING);
            return double_quote ? k_here_double : k_here_single;
        }
        if (current == '"' || current == '\'' || (current == '`' && dialect == Dialect::JULIA)) {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current);
            const bool character = current == '\'' && (dialect == Dialect::JULIA || dialect == Dialect::NIM);
            paint(context, {.begin = token_begin, .end = position}, character ? Style::CHARACTER : Style::STRING);
            paint_string_details(context, token_begin, position, dialect);
            continue;
        }
        const bool sigil = (current == '$' && (dialect == Dialect::PERL || dialect == Dialect::PHP || dialect == Dialect::POWERSHELL ||
                                               dialect == Dialect::R || dialect == Dialect::RUBY || dialect == Dialect::TCL)) ||
                           ((current == '@' || current == '%') && (dialect == Dialect::PERL || dialect == Dialect::RUBY));
        if (sigil) {
            const usize token_begin = position++;
            if (position < line_end && context.source[position] == '{') ++position;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            if (position < line_end && context.source[position] == '}') ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::VARIABLE);
            if (dialect == Dialect::POWERSHELL && command_position) command_position = false;
            continue;
        }
        if (dialect == Dialect::JULIA && current == '@') {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::ATTRIBUTE);
            continue;
        }
        if ((dialect == Dialect::RUBY || dialect == Dialect::REBOL) && current == ':' && position + 1 < line_end &&
            identifier_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::LABEL);
            continue;
        }
        const bool powershell_option =
            dialect == Dialect::POWERSHELL && current == '-' && position + 1 < line_end &&
            (identifier_start(context.source[position + 1]) ||
             (context.source[position + 1] == '-' && position + 2 < line_end && identifier_start(context.source[position + 2])));
        if (powershell_option) {
            const usize token_begin = position++;
            if (context.source[position] == '-') ++position;
            while (position < line_end && powershell_word_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, command_arguments ? Style::OPTION : Style::OPERATOR);
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
            while (position < line_end && (dialect == Dialect::POWERSHELL ? powershell_word_continue(context.source[position]) :
                                                                            identifier_continue(context.source[position])))
                ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const usize next = skip_space(context.source, position, line_end);
            const usize previous = previous_non_space(context.source, token_begin, begin);
            Style style = Style::IDENTIFIER;
            if (pending != Pending::NONE) {
                style = pending_style(pending);
                pending = Pending::NONE;
            } else if (contains(k_constants, word, dialect)) {
                style = Style::CONSTANT;
            } else if (contains(k_types, word, dialect)) {
                style = Style::TYPE;
            } else if (keyword(word, dialect)) {
                style = Style::KEYWORD;
                pending = declaration_after(word, dialect);
            } else if (next < line_end && context.source[next] == '(') {
                style = Style::FUNCTION;
            } else if (previous > begin && (context.source[previous - 1] == '.' || context.source[previous - 1] == '$')) {
                style = Style::PROPERTY;
            } else if (dialect == Dialect::POWERSHELL && command_position) {
                style = Style::FUNCTION;
            }
            paint(context, {.begin = token_begin, .end = position}, style);
            if (dialect == Dialect::POWERSHELL) {
                if (style == Style::KEYWORD && powershell_command_after(word)) {
                    command_position = true;
                    command_arguments = false;
                } else if (command_position) {
                    command_arguments = style == Style::FUNCTION;
                    command_position = false;
                }
            }
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < line_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            if (dialect == Dialect::POWERSHELL) {
                const auto value = context.source.substr(token_begin, position - token_begin);
                command_position = value == "=" || value.find_first_of("|;&{(") != std::string_view::npos;
                if (command_position) command_arguments = false;
            }
            continue;
        }
        ++position;
    }
    return k_normal;
}

} // namespace

void ScriptingLexer::lex(LexContext &context) const {
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
