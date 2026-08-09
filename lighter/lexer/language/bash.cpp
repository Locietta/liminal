#include "bash.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexBash.cxx and stlBash.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native port retains shell
// dialects, parameter expansion, command and function classification,
// multiline quotes, and heredocs while removing Scintilla's command-state,
// quote-stack, folding, and generated keyword infrastructure.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = BashLexer::Style;

constexpr auto k_keywords = make_word_set("break", "case", "continue", "coproc", "do", "done", "elif", "else", "esac", "eval", "exec",
                                          "exit", "export", "fi", "for", "function", "if", "in", "local", "readonly", "return", "select",
                                          "then", "time", "trap", "typeset", "until", "while");
constexpr auto k_builtins =
    make_word_set("alias", "bg", "bind", "builtin", "caller", "cd", "command", "compgen", "complete", "declare", "dirs", "disown", "echo",
                  "enable", "false", "fc", "fg", "getopts", "hash", "help", "history", "jobs", "kill", "let", "logout", "mapfile", "popd",
                  "printf", "pushd", "pwd", "read", "readarray", "set", "shift", "shopt", "source", "suspend", "test", "times", "true",
                  "type", "ulimit", "umask", "unalias", "unset", "wait");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_single_quote = 0x1000'0000;
constexpr u32 k_double_quote = 0x2000'0000;
constexpr u32 k_backtick = 0x3000'0000;
constexpr u32 k_heredoc = 0x4000'0000;
constexpr u32 k_heredoc_quoted = 0x0800'0000;
constexpr u32 k_heredoc_strip_tabs = 0x0400'0000;
constexpr u32 k_heredoc_hash_mask = 0x00ff'ffff;

[[nodiscard]] bool shell_word_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '-' || value == '.' || value == '/';
}

[[nodiscard]] bool shell_meta(char value) noexcept { return value <= ' ' || std::string_view("|&;()<>\"").contains(value); }

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("|&;()<>[]{}=!+*?~:,^").contains(value); }

[[nodiscard]] u32 heredoc_hash(std::string_view value) noexcept {
    u32 hash = 2'166'136'261u;
    hash ^= static_cast<u32>(value.size());
    hash *= 16'777'619u;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 16'777'619u;
    }
    return hash & k_heredoc_hash_mask;
}

[[nodiscard]] usize line_content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) noexcept { return slash + 1 < end ? slash + 2 : slash + 1; }

void paint_expansions(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\') {
            const usize token_end = escape_end(context.source, position, end);
            paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
            position = token_end;
            continue;
        }
        if (context.source[position] != '$') {
            ++position;
            continue;
        }

        const usize token_begin = position++;
        if (position >= end) {
            paint(context, {.begin = token_begin, .end = position}, Style::PARAMETER);
            continue;
        }
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
            continue;
        }
        if (context.source[position] == '(') {
            const usize delimiter_end = position + 1 < end && context.source[position + 1] == '(' ? position + 2 : position + 1;
            paint(context, {.begin = token_begin, .end = delimiter_end}, Style::OPERATOR);
            position = delimiter_end;
            continue;
        }
        if (ascii_identifier_start(context.source[position])) {
            ++position;
            while (position < end && ascii_identifier_continue(context.source[position])) ++position;
        } else {
            ++position;
        }
        paint(context, {.begin = token_begin, .end = position}, Style::VARIABLE);
    }
}

[[nodiscard]] u32 lex_quoted(LexContext &context, usize &position, usize end, u32 state) {
    const usize token_begin = position;
    const bool continuing = (state & k_state_mask) != k_normal;
    const u32 kind = continuing                       ? state & k_state_mask :
                     context.source[position] == '\'' ? k_single_quote :
                     context.source[position] == '"'  ? k_double_quote :
                                                        k_backtick;
    const char delimiter = kind == k_single_quote ? '\'' : kind == k_double_quote ? '"' : '`';
    if (!continuing) ++position;

    bool closed = false;
    while (position < end) {
        if (kind != k_single_quote && context.source[position] == '\\') {
            position = escape_end(context.source, position, end);
        } else if (context.source[position] == delimiter) {
            ++position;
            closed = true;
            break;
        } else {
            ++position;
        }
    }

    paint(context, {.begin = token_begin, .end = position}, Style::STRING);
    if (kind != k_single_quote) paint_expansions(context, token_begin, position);
    return closed ? k_normal : kind;
}

[[nodiscard]] u32 lex_heredoc_line(LexContext &context, usize begin, usize content_end, usize end, u32 state) {
    usize delimiter_begin = begin;
    if ((state & k_heredoc_strip_tabs) != 0) {
        while (delimiter_begin < content_end && context.source[delimiter_begin] == '\t') ++delimiter_begin;
    }
    const bool closes =
        heredoc_hash(context.source.substr(delimiter_begin, content_end - delimiter_begin)) == (state & k_heredoc_hash_mask);
    paint(context, {.begin = begin, .end = end}, closes ? Style::HEREDOC_DELIMITER : Style::HEREDOC);
    if (!closes && (state & k_heredoc_quoted) == 0) paint_expansions(context, begin, content_end);
    return closes ? k_normal : state;
}

[[nodiscard]] u32 heredoc_after(LexContext &context, usize &position, usize end) {
    const usize operator_begin = position;
    position += 2;
    bool strip_tabs = false;
    if (position < end && context.source[position] == '-') {
        strip_tabs = true;
        ++position;
    }
    paint(context, {.begin = operator_begin, .end = position}, Style::OPERATOR);
    while (position < end && (context.source[position] == ' ' || context.source[position] == '\t')) ++position;

    const usize styled_begin = position;
    char quote = '\0';
    if (position < end && (context.source[position] == '\'' || context.source[position] == '"')) quote = context.source[position++];
    const usize delimiter_begin = position;
    if (quote != '\0') {
        while (position < end && context.source[position] != quote) ++position;
    } else {
        while (position < end && !shell_meta(context.source[position])) ++position;
    }
    const usize delimiter_end = position;
    if (quote != '\0' && position < end) ++position;
    paint(context, {.begin = styled_begin, .end = position}, delimiter_begin == delimiter_end ? Style::ERROR : Style::HEREDOC_DELIMITER);
    if (delimiter_begin == delimiter_end) return k_normal;

    u32 state = k_heredoc | heredoc_hash(context.source.substr(delimiter_begin, delimiter_end - delimiter_begin));
    if (quote != '\0') state |= k_heredoc_quoted;
    if (strip_tabs) state |= k_heredoc_strip_tabs;
    return state;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 line_state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    const usize content_end = line_content_end(context.source, begin, end);
    if ((line_state & k_state_mask) == k_heredoc) return lex_heredoc_line(context, begin, content_end, end, line_state);

    usize position = begin;
    u32 quote_state = line_state & k_state_mask;
    u32 next_line_state = k_normal;
    bool command_position = true;
    bool function_name = false;
    while (position < content_end) {
        if (quote_state != k_normal) {
            quote_state = lex_quoted(context, position, content_end, quote_state);
            if (quote_state != k_normal) break;
            command_position = false;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
            continue;
        }
        const bool comment_start = current == '#' && (position == begin || ascii_space(context.source[position - 1]) ||
                                                      operator_character(context.source[position - 1]));
        if (comment_start) {
            paint(context, {.begin = position, .end = content_end}, Style::COMMENT);
            break;
        }
        if (current == '\'' || current == '"' || current == '`') {
            quote_state = lex_quoted(context, position, content_end, k_normal);
            command_position = false;
            continue;
        }
        if (current == '\\') {
            const usize token_end = escape_end(context.source, position, content_end);
            paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
            position = token_end;
            command_position = false;
            continue;
        }
        if (current == '$') {
            const usize token_begin = position;
            paint_expansions(context, position, content_end);
            if (context.styles[token_begin] == static_cast<u8>(Style::DEFAULT))
                paint(context, {.begin = token_begin, .end = token_begin + 1}, Style::VARIABLE);
            ++position;
            if (position < content_end && context.source[position] == '{') {
                usize depth = 1;
                ++position;
                while (position < content_end && depth != 0) {
                    if (context.source[position] == '{')
                        ++depth;
                    else if (context.source[position] == '}')
                        --depth;
                    ++position;
                }
            } else if (position < content_end && context.source[position] == '(') {
                position += position + 1 < content_end && context.source[position + 1] == '(' ? 2 : 1;
            } else {
                while (position < content_end && ascii_identifier_continue(context.source[position])) ++position;
            }
            command_position = false;
            continue;
        }
        if (context.source.substr(position).starts_with("<<")) {
            const u32 heredoc_state = heredoc_after(context, position, content_end);
            if (next_line_state == k_normal) next_line_state = heredoc_state;
            command_position = false;
            continue;
        }
        if (ascii_digit(current)) {
            const usize token_begin = position++;
            while (position < content_end && (ascii_alphanumeric(context.source[position]) || context.source[position] == '#')) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            command_position = false;
            continue;
        }
        if (shell_word_continue(current)) {
            const usize token_begin = position++;
            while (position < content_end && shell_word_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            usize next = position;
            while (next < content_end && (context.source[next] == ' ' || context.source[next] == '\t')) ++next;

            Style style = Style::IDENTIFIER;
            if (function_name || context.source.substr(next).starts_with("()")) {
                style = Style::FUNCTION;
                function_name = false;
            } else if (next < content_end && context.source[next] == '=' && !word.contains('/') && !word.contains('-')) {
                style = Style::VARIABLE;
            } else if (k_keywords.contains(word)) {
                style = Style::KEYWORD;
                function_name = word == "function";
            } else if (command_position || k_builtins.contains(word)) {
                style = Style::FUNCTION;
            } else if (word.starts_with('-')) {
                style = Style::OPTION;
            }
            paint(context, {.begin = token_begin, .end = position}, style);
            command_position = word == "then" || word == "do" || word == "else" || word == "elif";
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < content_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            command_position = context.source.substr(token_begin, position - token_begin).find_first_of("|&;(") != std::string_view::npos;
            continue;
        }
        ++position;
    }

    if (quote_state != k_normal) {
        if (end > content_end) paint(context, {.begin = content_end, .end = end}, Style::STRING);
        return quote_state;
    }
    return next_line_state;
}

} // namespace

void BashLexer::lex(LexContext &context) const {
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
