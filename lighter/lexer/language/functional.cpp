#include "functional.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexLisp.cxx, LexHaskell.cxx, LexOCaml.cxx,
// LexFSharp.cxx, LexErlang.cxx, LexElixir.cxx, and matching stl*.cpp data at
// revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native family keeps
// nested comment and heredoc checkpoints, declarations, constructors, atoms,
// aliases, attributes, macros, and parameters while omitting editor folding.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = FunctionalDialect;
using Style = FunctionalLexer::Style;

constexpr auto k_lisp_keywords =
    make_word_set("and", "block", "case", "catch", "cond", "defclass", "defconstant", "defmacro", "defmethod", "defpackage", "defparameter",
                  "defstruct", "deftype", "defun", "defvar", "do", "dolist", "dotimes", "else", "eval-when", "flet", "function", "go", "if",
                  "labels", "lambda", "let", "let*", "loop", "macrolet", "multiple-value-bind", "or", "progn", "quote", "return",
                  "return-from", "setq", "tagbody", "the", "throw", "unwind-protect", "when");
constexpr auto k_haskell_keywords =
    make_word_set("as", "case", "class", "data", "default", "deriving", "do", "else", "family", "forall", "foreign", "hiding", "if",
                  "import", "in", "infix", "infixl", "infixr", "instance", "let", "mdo", "module", "newtype", "of", "pattern", "qualified",
                  "rec", "role", "safe", "then", "type", "unsafe", "where");
constexpr auto k_ocaml_keywords =
    make_word_set("and", "as", "assert", "begin", "class", "constraint", "do", "done", "downto", "else", "end", "exception", "external",
                  "for", "fun", "function", "functor", "if", "in", "include", "inherit", "initializer", "lazy", "let", "match", "method",
                  "module", "mutable", "new", "nonrec", "object", "of", "open", "private", "rec", "sig", "struct", "then", "to", "try",
                  "type", "val", "virtual", "when", "while", "with");
constexpr auto k_fsharp_keywords =
    make_word_set("abstract", "and", "as", "assert", "base", "begin", "class", "default", "delegate", "do", "done", "downcast", "downto",
                  "elif", "else", "end", "exception", "extern", "false", "finally", "for", "fun", "function", "global", "if", "in",
                  "inherit", "inline", "interface", "internal", "lazy", "let", "match", "member", "module", "mutable", "namespace", "new",
                  "null", "of", "open", "override", "private", "public", "rec", "return", "static", "struct", "then", "to", "true", "try",
                  "type", "upcast", "use", "val", "void", "when", "while", "with", "yield");
constexpr auto k_erlang_keywords =
    make_word_set("after", "and", "andalso", "band", "begin", "bnot", "bor", "bsl", "bsr", "bxor", "case", "catch", "cond", "div", "end",
                  "fun", "if", "let", "maybe", "not", "of", "or", "orelse", "receive", "rem", "try", "when", "xor");
constexpr auto k_elixir_keywords = make_word_set(
    "after", "and", "case", "catch", "cond", "def", "defdelegate", "defexception", "defguard", "defimpl", "defmacro", "defmodule", "defp",
    "defprotocol", "defstruct", "do", "else", "end", "false", "fn", "for", "if", "import", "in", "nil", "not", "or", "quote", "raise",
    "receive", "require", "rescue", "super", "throw", "true", "try", "unless", "unquote", "use", "when", "with");
constexpr auto k_common_types = make_word_set("Atom", "Bool", "Boolean", "Char", "Float", "Integer", "List", "Map", "String", "Tuple",
                                              "Unit", "atom", "bool", "boolean", "byte", "char", "float", "int", "int32", "int64",
                                              "integer", "list", "map", "option", "result", "string", "unit");
constexpr auto k_constants = make_word_set("False", "None", "Nothing", "True", "false", "nil", "null", "true");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_nested_comment = 0x1000'0000;
constexpr u32 k_triple_double = 0x2000'0000;
constexpr u32 k_triple_single = 0x3000'0000;

enum struct Pending : u8 {
    NONE,
    TYPE,
    FUNCTION,
    MODULE,
    PROPERTY,
};

struct CommentDelimiters {
    std::string_view open;
    std::string_view close;
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

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '\'' || value == '?' || value == '!';
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\#").contains(value); }

[[nodiscard]] CommentDelimiters block_delimiters(Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::LISP: return {.open = "#|", .close = "|#"};
        case Dialect::HASKELL: return {.open = "{-", .close = "-}"};
        case Dialect::OCAML:
        case Dialect::FSHARP: return {.open = "(*", .close = "*)"};
        default: return {};
    }
}

[[nodiscard]] bool line_comment(std::string_view source, usize position, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::LISP: return source[position] == ';';
        case Dialect::HASKELL: return source.substr(position, 2) == "--";
        case Dialect::OCAML: return false;
        case Dialect::FSHARP: return source.substr(position, 2) == "//";
        case Dialect::ERLANG: return source[position] == '%';
        case Dialect::ELIXIR: return source[position] == '#';
    }
    return false;
}

[[nodiscard]] bool keyword(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::LISP: return k_lisp_keywords.contains(word);
        case Dialect::HASKELL: return k_haskell_keywords.contains(word);
        case Dialect::OCAML: return k_ocaml_keywords.contains(word);
        case Dialect::FSHARP: return k_fsharp_keywords.contains(word);
        case Dialect::ERLANG: return k_erlang_keywords.contains(word);
        case Dialect::ELIXIR: return k_elixir_keywords.contains(word);
    }
    return false;
}

[[nodiscard]] Pending declaration_after(std::string_view word, Dialect dialect) noexcept {
    if (dialect == Dialect::LISP) {
        if (word == "defclass" || word == "defstruct" || word == "deftype") return Pending::TYPE;
        if (word == "defmacro" || word == "defmethod" || word == "defun") return Pending::FUNCTION;
        if (word == "defpackage") return Pending::MODULE;
        if (word == "defconstant" || word == "defparameter" || word == "defvar") return Pending::PROPERTY;
    } else if (dialect == Dialect::HASKELL) {
        if (word == "class" || word == "data" || word == "family" || word == "newtype" || word == "type") return Pending::TYPE;
        if (word == "import" || word == "module") return Pending::MODULE;
    } else if (dialect == Dialect::OCAML || dialect == Dialect::FSHARP) {
        if (word == "class" || word == "exception" || word == "type") return Pending::TYPE;
        if (word == "let" || word == "member" || word == "method") return Pending::FUNCTION;
        if (word == "module" || word == "namespace" || word == "open") return Pending::MODULE;
    } else if (dialect == Dialect::ELIXIR) {
        if (word == "defmodule" || word == "defprotocol") return Pending::MODULE;
        if (word == "def" || word == "defdelegate" || word == "defguard" || word == "defmacro" || word == "defp") return Pending::FUNCTION;
        if (word == "defexception" || word == "defstruct") return Pending::TYPE;
    }
    return Pending::NONE;
}

[[nodiscard]] Style pending_style(Pending pending) noexcept {
    switch (pending) {
        case Pending::TYPE: return Style::TYPE;
        case Pending::FUNCTION: return Style::FUNCTION;
        case Pending::MODULE: return Style::MODULE;
        case Pending::PROPERTY: return Style::PROPERTY;
        case Pending::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
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

[[nodiscard]] u32 continue_comment(LexContext &context, usize &position, usize line_end, u32 state, Dialect dialect) {
    const CommentDelimiters delimiters = block_delimiters(dialect);
    u32 depth = std::max<u32>(1, state & k_payload_mask);
    const usize token_begin = position;
    while (position < line_end && depth != 0) {
        if (context.source.substr(position).starts_with(delimiters.open)) {
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
    return depth == 0 ? k_normal : k_nested_comment | depth;
}

[[nodiscard]] u32 continue_triple(LexContext &context, usize &position, usize line_end, u32 state) {
    const std::string_view delimiter = (state & k_state_mask) == k_triple_double ? "\"\"\"" : "'''";
    const usize token_begin = position;
    const usize close = context.source.find(delimiter, position);
    const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + delimiter.size();
    paint(context, {.begin = token_begin, .end = token_end}, Style::STRING);
    paint_escapes(context, token_begin, token_end);
    position = token_end;
    return close == std::string_view::npos || close >= line_end ? state : k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    if ((state & k_state_mask) == k_nested_comment) {
        state = continue_comment(context, position, line_end, state, dialect);
    } else if ((state & k_state_mask) == k_triple_double || (state & k_state_mask) == k_triple_single) {
        state = continue_triple(context, position, line_end, state);
    }
    if (state != k_normal) {
        if (end > line_end)
            paint(context, {.begin = line_end, .end = end}, (state & k_state_mask) == k_nested_comment ? Style::COMMENT : Style::STRING);
        return state;
    }

    Pending pending = Pending::NONE;
    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        if (line_comment(context.source, position, dialect)) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if (dialect == Dialect::HASKELL && context.source.substr(position).starts_with("{-#")) {
            const usize close = context.source.find("#-}", position + 3);
            const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + 3;
            paint(context, {.begin = position, .end = token_end}, Style::DIRECTIVE);
            position = token_end;
            continue;
        }
        const CommentDelimiters delimiters = block_delimiters(dialect);
        if (!delimiters.open.empty() && context.source.substr(position).starts_with(delimiters.open)) {
            const usize token_begin = position;
            position += delimiters.open.size();
            state = continue_comment(context, position, line_end, k_nested_comment | 1, dialect);
            paint(context, {.begin = token_begin, .end = token_begin + delimiters.open.size()}, Style::COMMENT);
            if (state != k_normal) return state;
            continue;
        }
        if ((dialect == Dialect::ELIXIR || dialect == Dialect::FSHARP) &&
            (context.source.substr(position).starts_with("\"\"\"") || context.source.substr(position).starts_with("'''"))) {
            const bool double_quote = current == '"';
            const usize token_begin = position;
            position += 3;
            state = continue_triple(context, position, line_end, double_quote ? k_triple_double : k_triple_single);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            if (state != k_normal) return state;
            continue;
        }
        if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current);
            const bool character = current == '\'' && dialect != Dialect::ERLANG && dialect != Dialect::ELIXIR;
            paint(context, {.begin = token_begin, .end = position}, character ? Style::CHARACTER : Style::STRING);
            paint_escapes(context, token_begin, position);
            continue;
        }
        if (dialect == Dialect::FSHARP && current == '#' && position == skip_space(context.source, begin, line_end)) {
            paint(context, {.begin = position, .end = line_end}, Style::DIRECTIVE);
            break;
        }
        if (dialect == Dialect::ERLANG && current == '-' && position + 1 < line_end && identifier_start(context.source[position + 1])) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  next < line_end && context.source[next] == '(' ? Style::DIRECTIVE : Style::OPERATOR);
            continue;
        }
        if (dialect == Dialect::ERLANG && current == '?') {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::DIRECTIVE);
            continue;
        }
        if ((dialect == Dialect::ELIXIR && (current == ':' || current == '@')) ||
            ((dialect == Dialect::OCAML || dialect == Dialect::FSHARP) && current == '~')) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, current == '@' ? Style::ATTRIBUTE : Style::LABEL);
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
            } else if (k_constants.contains(word)) {
                style = Style::CONSTANT;
            } else if (k_common_types.contains(word)) {
                style = Style::TYPE;
            } else if (keyword(word, dialect)) {
                style = Style::KEYWORD;
                pending = declaration_after(word, dialect);
            } else if ((dialect == Dialect::HASKELL || dialect == Dialect::OCAML || dialect == Dialect::FSHARP ||
                        dialect == Dialect::ELIXIR) &&
                       ascii_upper(word.front())) {
                style = dialect == Dialect::ELIXIR ? Style::MODULE : Style::TYPE;
            } else if (dialect == Dialect::ERLANG && ascii_upper(word.front())) {
                style = Style::PARAMETER;
            } else if ((next < line_end && context.source[next] == '(') ||
                       (dialect == Dialect::HASKELL && context.source.substr(next).starts_with("::")) ||
                       (dialect == Dialect::LISP && previous > begin && context.source[previous - 1] == '(')) {
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

void FunctionalLexer::lex(LexContext &context) const {
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
