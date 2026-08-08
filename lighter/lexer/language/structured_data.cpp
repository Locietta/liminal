#include "structured_data.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexJSON.cxx, LexTOML.cxx, LexYAML.cxx,
// LexConfig.cxx, LexProps.cxx, LexCSV.cxx, LexDiff.cxx, and their stl*.cpp
// language data at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0.
// The native port shares line traversal and token primitives while retaining
// distinct language identities, multiline strings, YAML block scalars, CSV
// records, properties, and diff line classification. Scintilla folding,
// mutable properties, and column-color editor features are omitted.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = StructuredDataDialect;
using Style = StructuredDataLexer::Style;

constexpr auto k_json_constants = make_word_set("Infinity", "NaN", "false", "null", "true");
constexpr auto k_toml_constants = make_word_set("false", "inf", "nan", "true");
constexpr auto k_yaml_constants =
    make_word_set(".inf", ".nan", "Inf", "NaN", "None", "false", "inf", "nan", "no", "none", "null", "off", "on", "true", "yes");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_json_block_comment = 0x1000'0000;
constexpr u32 k_json_double_string = 0x2000'0000;
constexpr u32 k_json_single_string = 0x3000'0000;
constexpr u32 k_toml_triple_double = 0x4000'0000;
constexpr u32 k_toml_triple_single = 0x5000'0000;
constexpr u32 k_csv_quoted = 0x6000'0000;
constexpr u32 k_yaml_block = 0x7000'0000;
constexpr u32 k_yaml_indent_mask = 0x0000'ffff;

[[nodiscard]] usize content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize skip_space(std::string_view source, usize position, usize end) noexcept {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] usize trim_end(std::string_view source, usize begin, usize end) noexcept {
    while (end > begin && (source[end - 1] == ' ' || source[end - 1] == '\t')) --end;
    return end;
}

[[nodiscard]] usize escape_end(std::string_view source, usize slash, usize end) noexcept {
    usize position = slash + 1;
    if (position >= end) return position;
    const char kind = source[position++];
    const usize digits = kind == 'x' ? 2 : kind == 'u' ? 4 : kind == 'U' ? 8 : 0;
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
        const usize token_end = escape_end(context.source, position, end);
        paint(context, {.begin = position, .end = token_end}, Style::ESCAPE);
        position = token_end;
    }
}

struct QuotedToken {
    usize end = 0;
    bool closed = false;
};

[[nodiscard]] QuotedToken quoted_end(std::string_view source, usize position, usize end, char quote, bool escapes) noexcept {
    while (position < end) {
        if (escapes && source[position] == '\\')
            position = escape_end(source, position, end);
        else if (source[position] == quote)
            return {.end = position + 1, .closed = true};
        else
            ++position;
    }
    return {.end = end};
}

[[nodiscard]] bool number_continue(char previous, char current) noexcept {
    if (ascii_alphanumeric(current) || current == '_' || current == '.' || current == ':') return true;
    return (current == '+' || current == '-') && (previous == 'e' || previous == 'E');
}

[[nodiscard]] usize number_end(std::string_view source, usize position, usize end) noexcept {
    ++position;
    while (position < end && number_continue(source[position - 1], source[position])) ++position;
    return position;
}

[[nodiscard]] u32 lex_json(LexContext &context, usize begin, usize end, u32 state) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    while (position < line_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_json_block_comment) {
            const usize found = context.source.find("*/", position);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) break;
            state = k_normal;
            continue;
        }
        if (kind == k_json_double_string || kind == k_json_single_string) {
            const char quote = kind == k_json_double_string ? '"' : '\'';
            const QuotedToken token = quoted_end(context.source, position, line_end, quote, true);
            paint(context, {.begin = position, .end = token.end}, Style::STRING);
            paint_escapes(context, position, token.end);
            position = token.end;
            if (!token.closed) break;
            state = k_normal;
            continue;
        }

        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
        } else if (context.source.substr(position).starts_with("//")) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        } else if (context.source.substr(position).starts_with("/*")) {
            state = k_json_block_comment;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            const QuotedToken token = quoted_end(context.source, position, line_end, current, true);
            position = token.end;
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  next < line_end && context.source[next] == ':' ? Style::KEY : Style::STRING);
            paint_escapes(context, token_begin, position);
            if (!token.closed) state = current == '"' ? k_json_double_string : k_json_single_string;
        } else if (ascii_digit(current) || current == '-' || current == '+') {
            const usize token_begin = position;
            position = number_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < line_end && ascii_identifier_continue(context.source[position])) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  k_json_constants.contains(word)                ? Style::KEYWORD :
                  next < line_end && context.source[next] == ':' ? Style::KEY :
                                                                   Style::ERROR);
        } else if (std::string_view("{}[],:.").contains(current)) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else {
            paint(context, {.begin = position, .end = position + 1}, Style::ERROR);
            ++position;
        }
    }
    if (end > line_end && (state & k_state_mask) != k_normal)
        paint(context, {.begin = line_end, .end = end}, (state & k_state_mask) == k_json_block_comment ? Style::COMMENT : Style::STRING);
    return state;
}

[[nodiscard]] u32 lex_toml(LexContext &context, usize begin, usize end, u32 state) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    const u32 initial_kind = state & k_state_mask;
    if (initial_kind == k_toml_triple_double || initial_kind == k_toml_triple_single) {
        const std::string_view delimiter = initial_kind == k_toml_triple_double ? "\"\"\"" : "'''";
        const usize found = context.source.find(delimiter, position);
        const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + delimiter.size();
        paint(context, {.begin = position, .end = token_end}, Style::BLOCK_STRING);
        if (initial_kind == k_toml_triple_double) paint_escapes(context, position, token_end);
        position = token_end;
        if (found == std::string_view::npos || found >= line_end) {
            if (end > line_end) paint(context, {.begin = line_end, .end = end}, Style::BLOCK_STRING);
            return state;
        }
        state = k_normal;
    }

    bool before_assignment = true;
    while (position < line_end) {
        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (current == '#') {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        } else if (before_assignment && current == '[') {
            const usize close = context.source.find(']', position + 1);
            const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + 1;
            paint(context, {.begin = position, .end = token_end}, Style::SECTION);
            position = token_end;
        } else if (context.source.substr(position).starts_with("\"\"\"") || context.source.substr(position).starts_with("'''")) {
            const bool double_quoted = current == '"';
            const std::string_view delimiter = double_quoted ? "\"\"\"" : "'''";
            const usize token_begin = position;
            const usize found = context.source.find(delimiter, position + 3);
            position = found == std::string_view::npos || found >= line_end ? line_end : found + 3;
            paint(context, {.begin = token_begin, .end = position}, Style::BLOCK_STRING);
            if (double_quoted) paint_escapes(context, token_begin, position);
            if (found == std::string_view::npos || found >= line_end) state = double_quoted ? k_toml_triple_double : k_toml_triple_single;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            const QuotedToken token = quoted_end(context.source, position, line_end, current, current == '"');
            position = token.end;
            paint(context, {.begin = token_begin, .end = position}, before_assignment ? Style::KEY : Style::STRING);
            if (current == '"') paint_escapes(context, token_begin, position);
        } else if (current == '=') {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
            before_assignment = false;
        } else if (ascii_digit(current) || (!before_assignment && (current == '+' || current == '-'))) {
            const usize token_begin = position;
            position = number_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (ascii_identifier_start(current) || current == '-') {
            const usize token_begin = position++;
            while (position < line_end && (ascii_identifier_continue(context.source[position]) || context.source[position] == '-'))
                ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            paint(context, {.begin = token_begin, .end = position},
                  before_assignment               ? Style::KEY :
                  k_toml_constants.contains(word) ? Style::KEYWORD :
                                                    Style::DEFAULT);
        } else if (std::string_view("[]{},.").contains(current)) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else {
            ++position;
        }
    }
    if (end > line_end && (state & k_state_mask) != k_normal) paint(context, {.begin = line_end, .end = end}, Style::BLOCK_STRING);
    return state;
}

[[nodiscard]] usize indentation(std::string_view source, usize begin, usize end) noexcept {
    usize count = 0;
    while (begin < end) {
        if (source[begin] == ' ')
            ++count;
        else if (source[begin] == '\t')
            count += 4;
        else
            break;
        ++begin;
    }
    return count;
}

[[nodiscard]] u32 lex_yaml(LexContext &context, usize begin, usize end, u32 state) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = skip_space(context.source, begin, line_end);
    const bool blank = position == line_end;
    if ((state & k_state_mask) == k_yaml_block) {
        const usize parent_indent = state & k_yaml_indent_mask;
        if (blank || indentation(context.source, begin, line_end) > parent_indent) {
            paint(context, {.begin = begin, .end = end}, Style::BLOCK_STRING);
            return state;
        }
        state = k_normal;
    }
    if (blank) return state;
    if (context.source.substr(position, 3) == "---" || context.source.substr(position, 3) == "...") {
        paint(context, {.begin = position, .end = line_end}, Style::SECTION);
        return state;
    }
    if (context.source[position] == '%') {
        paint(context, {.begin = position, .end = line_end}, Style::DIRECTIVE);
        return state;
    }

    while (position < line_end) {
        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (current == '#') {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            const QuotedToken token = quoted_end(context.source, position, line_end, current, current == '"');
            position = token.end;
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  next < line_end && context.source[next] == ':' ? Style::KEY : Style::STRING);
            if (current == '"') paint_escapes(context, token_begin, position);
        } else if (current == '&' || current == '*') {
            const usize token_begin = position++;
            while (position < line_end && !ascii_space(context.source[position]) &&
                   !std::string_view(",[]{}").contains(context.source[position]))
                ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::ANCHOR);
        } else if (current == '!') {
            const usize token_begin = position++;
            while (position < line_end && !ascii_space(context.source[position]) &&
                   !std::string_view(",[]{}").contains(context.source[position]))
                ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::TAG);
        } else if (current == '|' || current == '>') {
            const usize token_begin = position++;
            while (position < line_end && std::string_view("+-0123456789").contains(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            state = k_yaml_block | static_cast<u32>(std::min<usize>(indentation(context.source, begin, line_end), k_yaml_indent_mask));
        } else if (ascii_digit(current) ||
                   ((current == '-' || current == '+') && position + 1 < line_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position;
            position = number_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (!ascii_space(current) && !std::string_view("[]{},:").contains(current)) {
            const usize token_begin = position++;
            while (position < line_end && !ascii_space(context.source[position]) &&
                   !std::string_view("[]{},:#").contains(context.source[position]))
                ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            const usize next = skip_space(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position},
                  next < line_end && context.source[next] == ':' ? Style::KEY :
                  k_yaml_constants.contains(word)                ? Style::KEYWORD :
                                                                   Style::DEFAULT);
        } else {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        }
    }
    return state;
}

void lex_property_value(LexContext &context, usize position, usize end) {
    while (position < end) {
        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            const QuotedToken token = quoted_end(context.source, position, end, current, true);
            position = token.end;
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_escapes(context, token_begin, position);
        } else if (ascii_digit(current) ||
                   ((current == '-' || current == '+') && position + 1 < end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position;
            position = number_end(context.source, position, end);
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        } else if (current == '$' || current == '%') {
            const usize token_begin = position++;
            if (position < end && (context.source[position] == '{' || context.source[position] == '(')) {
                const char close = context.source[position] == '{' ? '}' : ')';
                ++position;
                while (position < end && context.source[position] != close) ++position;
                if (position < end) ++position;
            } else {
                while (position < end && ascii_identifier_continue(context.source[position])) ++position;
            }
            paint(context, {.begin = token_begin, .end = position}, Style::ANCHOR);
        } else if (ascii_identifier_start(current)) {
            const usize token_begin = position++;
            while (position < end && (ascii_identifier_continue(context.source[position]) || context.source[position] == '-')) ++position;
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            paint(context, {.begin = token_begin, .end = position},
                  k_yaml_constants.contains(word) || k_toml_constants.contains(word) ? Style::KEYWORD : Style::DEFAULT);
        } else if (std::string_view("{}[](),.;").contains(current)) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] u32 lex_properties(LexContext &context, usize begin, usize end, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = skip_space(context.source, begin, line_end);
    if (position == line_end) return k_normal;
    if (context.source[position] == '#' || context.source[position] == ';' ||
        (dialect == Dialect::INI && context.source[position] == '!')) {
        paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
        return k_normal;
    }
    if (context.source[position] == '[') {
        paint(context, {.begin = position, .end = line_end}, Style::SECTION);
        return k_normal;
    }
    if (dialect == Dialect::CONFIG && context.source[position] == '<') {
        paint(context, {.begin = position, .end = line_end}, Style::SECTION);
        return k_normal;
    }

    usize separator = position;
    bool quoted = false;
    char quote = '\0';
    while (separator < line_end) {
        const char current = context.source[separator];
        if (quoted) {
            if (current == '\\')
                separator = escape_end(context.source, separator, line_end);
            else {
                if (current == quote) quoted = false;
                ++separator;
            }
        } else if (current == '"' || current == '\'') {
            quoted = true;
            quote = current;
            ++separator;
        } else if (current == '=' || current == ':') {
            break;
        } else {
            ++separator;
        }
    }
    if (separator < line_end) {
        const usize key_end = trim_end(context.source, position, separator);
        paint(context, {.begin = position, .end = key_end}, Style::KEY);
        paint(context, {.begin = separator, .end = separator + 1}, Style::OPERATOR);
        lex_property_value(context, separator + 1, line_end);
    } else if (dialect == Dialect::CONFIG) {
        usize key_end = position;
        while (key_end < line_end && !ascii_space(context.source[key_end])) ++key_end;
        paint(context, {.begin = position, .end = key_end}, Style::DIRECTIVE);
        lex_property_value(context, key_end, line_end);
    } else {
        paint(context, {.begin = position, .end = line_end}, Style::DEFAULT);
    }
    return k_normal;
}

[[nodiscard]] u32 lex_csv(LexContext &context, usize begin, usize end, u32 state, char delimiter) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    bool quoted = (state & k_state_mask) == k_csv_quoted;
    while (position < line_end) {
        if (quoted) {
            const usize token_begin = position;
            while (position < line_end) {
                if (context.source[position] != '"') {
                    ++position;
                } else if (position + 1 < line_end && context.source[position + 1] == '"') {
                    position += 2;
                } else {
                    ++position;
                    quoted = false;
                    break;
                }
            }
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
        } else if (context.source[position] == delimiter) {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else if (context.source[position] == '"') {
            quoted = true;
            paint(context, {.begin = position, .end = position + 1}, Style::STRING);
            ++position;
        } else {
            const usize token_begin = position;
            while (position < line_end && context.source[position] != delimiter) ++position;
            const std::string_view field = context.source.substr(token_begin, position - token_begin);
            const bool numeric = !field.empty() && (ascii_digit(field.front()) || field.front() == '-' || field.front() == '+');
            if (numeric) paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
        }
    }
    if (quoted && end > line_end) paint(context, {.begin = line_end, .end = end}, Style::STRING);
    return quoted ? k_csv_quoted : k_normal;
}

[[nodiscard]] u32 lex_diff(LexContext &context, usize begin, usize end) {
    const usize line_end = content_end(context.source, begin, end);
    const std::string_view line = context.source.substr(begin, line_end - begin);
    Style style = Style::DEFAULT;
    if (line.starts_with("diff ") || line.starts_with("Index: ") || line.starts_with("--- ") || line.starts_with("+++ ") ||
        line.starts_with("====") || line.starts_with("*** "))
        style = Style::SECTION;
    else if (line.starts_with("@@") || line.starts_with("********") || (!line.empty() && ascii_digit(line.front())))
        style = Style::ANCHOR;
    else if (line.starts_with('+'))
        style = Style::ADDED;
    else if (line.starts_with('-') || line.starts_with('<'))
        style = Style::REMOVED;
    else if (line.starts_with('>'))
        style = Style::ADDED;
    else if (line.starts_with("\\ No newline"))
        style = Style::COMMENT;
    paint(context, {.begin = begin, .end = end}, style);
    return k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    switch (dialect) {
        case Dialect::JSON: return lex_json(context, begin, end, state);
        case Dialect::TOML: return lex_toml(context, begin, end, state);
        case Dialect::YAML: return lex_yaml(context, begin, end, state);
        case Dialect::CONFIG:
        case Dialect::INI: return lex_properties(context, begin, end, dialect);
        case Dialect::CSV: return lex_csv(context, begin, end, state, ',');
        case Dialect::TSV: return lex_csv(context, begin, end, state, '\t');
        case Dialect::DIFF: return lex_diff(context, begin, end);
    }
    return k_normal;
}

} // namespace

void StructuredDataLexer::lex(LexContext &context) const {
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
