#include "sql.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexSQL.cxx and stlSQL.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. The native port retains SQL
// keywords and types, declarations, table and column roles, dialect-neutral
// quoted identifiers, parameters, nested comments, and dollar strings while
// removing folding and Scintilla properties.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Style = SqlLexer::Style;

constexpr auto k_keywords =
    make_word_set("add", "all", "alter", "and", "as", "asc", "begin", "between", "by", "case", "check", "column", "commit", "constraint",
                  "create", "cross", "database", "default", "delete", "desc", "distinct", "drop", "else", "end", "except", "exists",
                  "false", "fetch", "for", "foreign", "from", "full", "function", "grant", "group", "having", "if", "in", "index", "inner",
                  "insert", "intersect", "into", "is", "join", "key", "left", "like", "limit", "merge", "not", "null", "offset", "on", "or",
                  "order", "outer", "primary", "procedure", "references", "return", "revoke", "right", "rollback", "row", "schema",
                  "select", "set", "table", "then", "true", "union", "unique", "update", "use", "values", "view", "when", "where", "with");
constexpr auto k_types =
    make_word_set("bigint", "binary", "bit", "blob", "boolean", "char", "clob", "date", "datetime", "decimal", "double", "enum", "float",
                  "int", "integer", "interval", "json", "money", "nchar", "numeric", "nvarchar", "real", "serial", "smallint", "text",
                  "time", "timestamp", "tinyint", "uuid", "varbinary", "varchar", "xml");
constexpr auto k_functions =
    make_word_set("avg", "cast", "coalesce", "concat", "count", "current_date", "current_timestamp", "extract", "greatest", "least",
                  "lower", "max", "min", "nullif", "replace", "round", "substring", "sum", "trim", "upper");
constexpr auto k_constants = make_word_set("false", "null", "true", "unknown");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_single_string = 0x2000'0000;
constexpr u32 k_dollar_string = 0x3000'0000;

enum struct PendingDeclaration : u8 {
    NONE,
    MODULE,
    FUNCTION,
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

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_' || value == '#'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '$' || value == '#';
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|").contains(value); }

[[nodiscard]] std::string lower_ascii(std::string_view word) {
    std::string lowered(word);
    std::ranges::transform(lowered, lowered.begin(), ascii_to_lower);
    return lowered;
}

[[nodiscard]] PendingDeclaration declaration_after(std::string_view word) noexcept {
    if (word == "database" || word == "from" || word == "into" || word == "join" || word == "schema" || word == "table" ||
        word == "update" || word == "use" || word == "view")
        return PendingDeclaration::MODULE;
    if (word == "function" || word == "procedure") return PendingDeclaration::FUNCTION;
    return PendingDeclaration::NONE;
}

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote, char close) noexcept {
    while (position < end) {
        if (source[position] == close) {
            if (position + 1 < end && source[position + 1] == close && quote != '[')
                position += 2;
            else
                return position + 1;
        } else if (source[position] == '\\' && position + 1 < end) {
            position += 2;
        } else {
            ++position;
        }
    }
    return position;
}

void paint_string_escapes(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E');
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    PendingDeclaration pending = PendingDeclaration::NONE;
    while (position < line_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_block_comment) {
            usize depth = std::max<u32>(1, state & k_payload_mask);
            const usize token_begin = position;
            while (position < line_end) {
                if (context.source.substr(position).starts_with("/*")) {
                    ++depth;
                    position += 2;
                } else if (context.source.substr(position).starts_with("*/")) {
                    position += 2;
                    if (--depth == 0) break;
                } else {
                    ++position;
                }
            }
            paint(context, {.begin = token_begin, .end = position}, Style::COMMENT);
            state = depth == 0 ? k_normal : k_block_comment | static_cast<u32>(std::min<usize>(depth, k_payload_mask));
            if (state != k_normal) break;
            continue;
        }
        if (kind == k_single_string) {
            const usize token_begin = position;
            position = quoted_end(context.source, position, line_end, '\'', '\'');
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_string_escapes(context, token_begin, position);
            if (position == line_end && (position == token_begin || context.source[position - 1] != '\'')) break;
            state = k_normal;
            continue;
        }
        if (kind == k_dollar_string) {
            const usize found = context.source.find("$$", position);
            position = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = begin, .end = position}, Style::STRING);
            if (found == std::string_view::npos || found >= line_end) break;
            state = k_normal;
            continue;
        }

        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (context.source.substr(position).starts_with("--") || current == '#') {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        } else if (context.source.substr(position).starts_with("/*")) {
            paint(context, {.begin = position, .end = position + 2}, Style::COMMENT);
            position += 2;
            state = k_block_comment | 1;
        } else if (context.source.substr(position).starts_with("$$")) {
            const usize token_begin = position;
            position += 2;
            const usize found = context.source.find("$$", position);
            position = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            if (found == std::string_view::npos || found >= line_end) state = k_dollar_string;
        } else if (current == '\'') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, line_end, current, current);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_string_escapes(context, token_begin, position);
            if (position == line_end && context.source[position - 1] != current) state = k_single_string;
        } else if (current == '"' || current == '`' || current == '[') {
            const usize token_begin = position++;
            const char close = current == '[' ? ']' : current;
            position = quoted_end(context.source, position, line_end, current, close);
            paint(context, {.begin = token_begin, .end = position}, Style::QUOTED_IDENTIFIER);
        } else if ((current == ':' || current == '@' || current == '$') && position + 1 < line_end &&
                   (ascii_digit(context.source[position + 1]) || identifier_start(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::PARAMETER);
        } else if (ascii_digit(current) || (current == '.' && position + 1 < line_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < line_end && number_continue(context.source[position - 1], context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (identifier_start(current)) {
            const usize token_begin = position++;
            while (position < line_end && identifier_continue(context.source[position])) ++position;
            const std::string lowered = lower_ascii(context.source.substr(token_begin, position - token_begin));
            Style style = Style::IDENTIFIER;
            if (pending != PendingDeclaration::NONE) {
                style = pending == PendingDeclaration::MODULE ? Style::MODULE : Style::FUNCTION;
                pending = PendingDeclaration::NONE;
            } else if (k_constants.contains(lowered)) {
                style = Style::CONSTANT;
            } else if (k_types.contains(lowered)) {
                style = Style::TYPE;
            } else if (k_keywords.contains(lowered)) {
                style = Style::KEYWORD;
                pending = declaration_after(lowered);
            } else if (k_functions.contains(lowered)) {
                style = Style::FUNCTION;
            } else {
                const usize previous = previous_non_space(context.source, token_begin, begin);
                const usize next = skip_space(context.source, position, line_end);
                if (next < line_end && context.source[next] == '(')
                    style = Style::FUNCTION;
                else if (previous > begin && context.source[previous - 1] == '.')
                    style = Style::PROPERTY;
            }
            paint(context, {.begin = token_begin, .end = position}, style);
        } else if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < line_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
        } else {
            ++position;
        }
    }
    if (end > line_end && (state & k_state_mask) != k_normal)
        paint(context, {.begin = line_end, .end = end}, (state & k_state_mask) == k_block_comment ? Style::COMMENT : Style::STRING);
    return state;
}

} // namespace

void SqlLexer::lex(LexContext &context) const {
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
