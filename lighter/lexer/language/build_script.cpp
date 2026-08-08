#include "build_script.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexBatch.cxx, LexCMake.cxx, LexGN.cxx,
// LexMake.cxx, LexJam.cxx, LexInno.cxx, LexNsis.cxx, and matching stl*.cpp
// data at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0.
// This native family retains each language identity, command and variable
// roles, labels/sections, CMake bracket constructs, and multiline comments.
// Scintilla folding and editor properties are omitted.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = BuildScriptDialect;
using Style = BuildScriptLexer::Style;

constexpr auto k_batch_keywords = make_word_set(
    "call", "cd", "chdir", "choice", "cls", "copy", "del", "dir", "do", "echo", "else", "endlocal", "equ", "errorlevel", "exist", "exit",
    "for", "geq", "goto", "gtr", "if", "in", "leq", "lss", "md", "mkdir", "move", "neq", "not", "pause", "popd", "pushd", "rd", "rem",
    "ren", "rename", "rmdir", "set", "setlocal", "shift", "start", "title", "type", "ver", "verify");
constexpr auto k_cmake_keywords =
    make_word_set("block", "break", "cmake_language", "continue", "else", "elseif", "endblock", "endforeach", "endfunction", "endif",
                  "endmacro", "endwhile", "foreach", "function", "if", "macro", "return", "while");
constexpr auto k_cmake_commands =
    make_word_set("add_custom_command", "add_custom_target", "add_definitions", "add_executable", "add_library", "add_subdirectory",
                  "cmake_minimum_required", "configure_file", "enable_language", "find_file", "find_library", "find_package", "find_path",
                  "find_program", "include", "install", "link_libraries", "list", "message", "option", "project", "set", "set_property",
                  "string", "target_compile_definitions", "target_compile_features", "target_compile_options", "target_include_directories",
                  "target_link_libraries", "target_sources", "unset");
constexpr auto k_gn_keywords = make_word_set("else", "false", "foreach", "if", "true");
constexpr auto k_gn_functions =
    make_word_set("assert", "config", "copy", "declare_args", "defined", "exec_script", "executable", "forward_variables_from",
                  "get_label_info", "get_path_info", "group", "import", "loadable_module", "print", "process_file_template", "read_file",
                  "set_default_toolchain", "shared_library", "source_set", "static_library", "template", "tool", "toolchain");
constexpr auto k_make_keywords = make_word_set("define", "else", "endef", "endif", "export", "if", "ifdef", "ifeq", "ifndef", "ifneq",
                                               "include", "override", "private", "sinclude", "undefine", "unexport", "vpath");
constexpr auto k_make_functions = make_word_set(
    "abspath", "addprefix", "addsuffix", "and", "basename", "call", "dir", "error", "eval", "file", "filter", "filter-out", "findstring",
    "firstword", "flavor", "foreach", "guile", "if", "info", "join", "lastword", "notdir", "or", "origin", "patsubst", "realpath", "shell",
    "sort", "strip", "subst", "suffix", "value", "warning", "wildcard", "word", "wordlist", "words");
constexpr auto k_jam_keywords =
    make_word_set("actions", "bind", "break", "case", "class", "continue", "else", "existing", "for", "if", "ignore", "in", "include",
                  "local", "module", "on", "piecemeal", "quietly", "return", "rule", "switch", "together", "updated", "while");
constexpr auto k_inno_keywords =
    make_word_set("and", "begin", "case", "const", "do", "downto", "else", "end", "except", "finally", "for", "function", "if", "not", "of",
                  "or", "procedure", "repeat", "then", "to", "try", "type", "until", "var", "while", "with");
constexpr auto k_nsis_keywords =
    make_word_set("abort", "bringtofront", "call", "callinstdll", "clearerrors", "copyfiles", "createdirectory", "createfont", "createlink",
                  "delete", "detailprint", "exec", "execshell", "execwait", "file", "fileclose", "fileopen", "fileread", "filewrite",
                  "findclose", "findfirst", "findnext", "function", "functionend", "goto", "ifabort", "iferrors", "iffileexists", "ifthen",
                  "insttype", "messagebox", "page", "pop", "push", "quit", "readenvstr", "rename", "return", "rmdir", "section",
                  "sectionend", "setoutpath", "sleep", "strcmp", "strcpy", "writeinistr", "writeregstr");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_bracket_comment = 0x2000'0000;
constexpr u32 k_bracket_string = 0x3000'0000;
constexpr u32 k_double_string = 0x4000'0000;

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
    return dialect == Dialect::BATCH || dialect == Dialect::INNO_SETUP || dialect == Dialect::NSIS;
}

template <usize Size>
[[nodiscard]] bool contains(const WordSet<Size> &set, std::string_view word, Dialect dialect) noexcept {
    return insensitive(dialect) ? contains_ascii_ci(set, word) : set.contains(word);
}

[[nodiscard]] bool keyword(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::BATCH: return contains(k_batch_keywords, word, dialect);
        case Dialect::CMAKE: return k_cmake_keywords.contains(word);
        case Dialect::GN: return k_gn_keywords.contains(word);
        case Dialect::MAKE: return k_make_keywords.contains(word);
        case Dialect::JAM: return k_jam_keywords.contains(word);
        case Dialect::INNO_SETUP: return contains(k_inno_keywords, word, dialect);
        case Dialect::NSIS: return contains(k_nsis_keywords, word, dialect);
    }
    return false;
}

[[nodiscard]] bool builtin(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::CMAKE: return k_cmake_commands.contains(word);
        case Dialect::GN: return k_gn_functions.contains(word);
        case Dialect::MAKE: return k_make_functions.contains(word);
        default: return false;
    }
}

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_' || value == '.'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '-' || value == '.';
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\").contains(value); }

struct BracketDelimiter {
    usize length = 0;
    usize hashes = 0;
};

[[nodiscard]] BracketDelimiter bracket_opener(std::string_view source, usize position, usize end) noexcept {
    if (position >= end || source[position] != '[') return {};
    usize cursor = position + 1;
    while (cursor < end && source[cursor] == '=') ++cursor;
    if (cursor >= end || source[cursor] != '[') return {};
    return {.length = cursor - position + 1, .hashes = cursor - position - 1};
}

[[nodiscard]] usize find_bracket_close(std::string_view source, usize position, usize end, usize hashes) noexcept {
    while (position < end) {
        const usize close = source.find(']', position);
        if (close == std::string_view::npos || close >= end) return end;
        usize cursor = close + 1;
        usize count = 0;
        while (cursor < end && source[cursor] == '=' && count < hashes) {
            ++cursor;
            ++count;
        }
        if (count == hashes && cursor < end && source[cursor] == ']') return cursor + 1;
        position = close + 1;
    }
    return end;
}

[[nodiscard]] bool bracket_closed_at(std::string_view source, usize end, usize hashes) noexcept {
    if (end < hashes + 2 || source[end - 1] != ']') return false;
    usize position = end - 2;
    for (usize count = 0; count < hashes; ++count) {
        if (source[position] != '=') return false;
        --position;
    }
    return source[position] == ']';
}

[[nodiscard]] usize variable_end(std::string_view source, usize position, usize end, Dialect dialect) noexcept {
    const usize begin = position++;
    if (begin >= end) return end;
    if (dialect == Dialect::BATCH && (source[begin] == '%' || source[begin] == '!')) {
        const usize close = source.find(source[begin], position);
        return close == std::string_view::npos || close >= end ? end : close + 1;
    }
    if (position < end && (source[position] == '{' || source[position] == '(' || source[position] == '<')) {
        const char open = source[position++];
        const char close = open == '{' ? '}' : open == '(' ? ')' : '>';
        usize depth = 1;
        while (position < end && depth != 0) {
            if (source[position] == open)
                ++depth;
            else if (source[position] == close)
                --depth;
            ++position;
        }
        return position;
    }
    while (position < end && (identifier_continue(source[position]) || ascii_digit(source[position]))) ++position;
    return position;
}

void paint_string_details(LexContext &context, usize begin, usize end, Dialect dialect) {
    for (usize position = begin; position < end;) {
        const char current = context.source[position];
        if (current == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else if (current == '$' || (dialect == Dialect::BATCH && (current == '%' || current == '!'))) {
            const usize token_end = variable_end(context.source, position, end, dialect);
            paint(context, {.begin = position, .end = token_end}, Style::PARAMETER);
            position = token_end;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] u32 continue_state(LexContext &context, usize &position, usize line_end, u32 state, Dialect dialect) {
    const u32 kind = state & k_state_mask;
    if (kind == k_block_comment) {
        std::string_view close = dialect == Dialect::INNO_SETUP ? "}" : "*/";
        const usize found = context.source.find(close, position);
        const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + close.size();
        paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
        position = token_end;
        return found == std::string_view::npos || found >= line_end ? state : k_normal;
    }
    if (kind == k_bracket_comment || kind == k_bracket_string) {
        const usize hashes = (state & k_payload_mask) - 1;
        const usize token_end = find_bracket_close(context.source, position, line_end, hashes);
        paint(context, {.begin = position, .end = token_end}, kind == k_bracket_comment ? Style::COMMENT : Style::STRING);
        position = token_end;
        return token_end == line_end && !bracket_closed_at(context.source, token_end, hashes) ? state : k_normal;
    }
    if (kind == k_double_string) {
        const usize token_begin = position;
        while (position < line_end) {
            if (context.source[position] == '\\' && position + 1 < line_end) {
                position += 2;
            } else if (context.source[position++] == '"') {
                paint(context, {.begin = token_begin, .end = position}, Style::STRING);
                paint_string_details(context, token_begin, position, dialect);
                return k_normal;
            }
        }
        paint(context, {.begin = token_begin, .end = position}, Style::STRING);
        paint_string_details(context, token_begin, position, dialect);
        return state;
    }
    return k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    if ((state & k_state_mask) != k_normal) {
        state = continue_state(context, position, line_end, state, dialect);
        if (state != k_normal) {
            if (end > line_end)
                paint(context, {.begin = line_end, .end = end},
                      (state & k_state_mask) == k_block_comment || (state & k_state_mask) == k_bracket_comment ? Style::COMMENT :
                                                                                                                 Style::STRING);
            return state;
        }
    }

    const usize first = skip_space(context.source, position, line_end);
    if (first >= line_end) return k_normal;
    if (dialect == Dialect::BATCH) {
        if (context.source.substr(first, 2) == "::" ||
            (context.source.substr(first).size() >= 3 && equal_ascii_ci(context.source.substr(first, 3), "rem") &&
             (first + 3 == line_end || ascii_space(context.source[first + 3])))) {
            paint(context, {.begin = first, .end = line_end}, Style::COMMENT);
            return k_normal;
        }
        if (context.source[first] == ':' && (first + 1 >= line_end || context.source[first + 1] != ':')) {
            paint(context, {.begin = first, .end = line_end}, Style::LABEL);
            return k_normal;
        }
    }
    if (dialect == Dialect::INNO_SETUP && context.source[first] == '[') {
        const usize close = context.source.find(']', first + 1);
        if (close != std::string_view::npos && close < line_end) {
            paint(context, {.begin = first, .end = close + 1}, Style::SECTION);
            return k_normal;
        }
    }
    if (dialect == Dialect::MAKE && first == begin && context.source[first] != '\t') {
        const usize colon = context.source.find(':', first);
        const usize equals = context.source.find('=', first);
        if (colon != std::string_view::npos && colon < line_end && (equals == std::string_view::npos || colon < equals))
            paint(context, {.begin = first, .end = colon}, Style::LABEL);
    }

    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        const bool hash_comment = current == '#' && dialect != Dialect::BATCH && dialect != Dialect::INNO_SETUP;
        const bool semicolon_comment = current == ';' && (dialect == Dialect::INNO_SETUP || dialect == Dialect::NSIS);
        if (hash_comment || semicolon_comment || (dialect == Dialect::INNO_SETUP && context.source.substr(position, 2) == "//")) {
            if (dialect == Dialect::CMAKE && current == '#') {
                const BracketDelimiter delimiter = bracket_opener(context.source, position + 1, line_end);
                if (delimiter.length != 0) {
                    const usize token_end = find_bracket_close(context.source, position + 1 + delimiter.length, line_end, delimiter.hashes);
                    paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
                    if (token_end == line_end && !bracket_closed_at(context.source, token_end, delimiter.hashes))
                        return k_bracket_comment | static_cast<u32>(delimiter.hashes + 1);
                    position = token_end;
                    continue;
                }
            }
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if (context.source.substr(position, 2) == "/*" && (dialect == Dialect::GN || dialect == Dialect::NSIS)) {
            const usize found = context.source.find("*/", position + 2);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) return k_block_comment;
            continue;
        }
        if (current == '{' && dialect == Dialect::INNO_SETUP) {
            const usize found = context.source.find('}', position + 1);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + 1;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) return k_block_comment;
            continue;
        }
        if (current == '[' && dialect == Dialect::CMAKE) {
            const BracketDelimiter delimiter = bracket_opener(context.source, position, line_end);
            if (delimiter.length != 0) {
                const usize token_end = find_bracket_close(context.source, position + delimiter.length, line_end, delimiter.hashes);
                paint(context, {.begin = position, .end = token_end}, Style::STRING);
                position = token_end;
                if (token_end == line_end && !bracket_closed_at(context.source, token_end, delimiter.hashes))
                    return k_bracket_string | static_cast<u32>(delimiter.hashes + 1);
                continue;
            }
        }
        if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            bool closed = false;
            while (position < line_end) {
                if (context.source[position] == '\\' && position + 1 < line_end)
                    position += 2;
                else if (context.source[position++] == current) {
                    closed = true;
                    break;
                }
            }
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_string_details(context, token_begin, position, dialect);
            if (!closed && current == '"' && dialect == Dialect::CMAKE) return k_double_string;
            continue;
        }
        if (current == '$' || (dialect == Dialect::BATCH && (current == '%' || current == '!'))) {
            const usize token_end = variable_end(context.source, position, line_end, dialect);
            paint(context, {.begin = position, .end = token_end}, Style::VARIABLE);
            position = token_end;
            continue;
        }
        if (current == '@' && dialect == Dialect::BATCH) {
            paint(context, {.begin = position, .end = position + 1}, Style::DIRECTIVE);
            ++position;
            continue;
        }
        if (current == '#' && dialect == Dialect::INNO_SETUP) {
            paint(context, {.begin = position, .end = line_end}, Style::DIRECTIVE);
            break;
        }
        if (ascii_digit(current)) {
            const usize token_begin = position++;
            while (position < line_end && (ascii_alphanumeric(context.source[position]) || context.source[position] == '.')) ++position;
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
            if (dialect == Dialect::MAKE && next < line_end && context.source[next] == ':')
                style = Style::LABEL;
            else if (keyword(word, dialect))
                style = Style::KEYWORD;
            else if (builtin(word, dialect) || (next < line_end && context.source[next] == '('))
                style = Style::FUNCTION;
            else if ((dialect == Dialect::INNO_SETUP || dialect == Dialect::GN || dialect == Dialect::MAKE) && next < line_end &&
                     context.source[next] == '=')
                style = Style::PROPERTY;
            else if (previous > begin && context.source[previous - 1] == '.')
                style = Style::PROPERTY;
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

void BuildScriptLexer::lex(LexContext &context) const {
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
