#include "markup.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>

// Derived from Notepad4's LexHTML.cxx, LexMarkdown.cxx, stlHTML.cpp,
// stlXML.cpp, and stlMarkdown.cpp at revision
// eee400c824b30e0aa41ef06a18ce22cf69b5cbb0. This native port retains
// multiline comments, CDATA, tag and attribute classification, entities,
// Markdown blocks, links, inline code, and streamed fences while removing
// embedded-editor modes, folding, and Scintilla document access.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = MarkupDialect;
using Style = MarkupLexer::Style;

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_normal = 0;
constexpr u32 k_comment = 0x1000'0000;
constexpr u32 k_cdata = 0x2000'0000;
constexpr u32 k_tag = 0x3000'0000;
constexpr u32 k_markdown_fence = 0x4000'0000;
constexpr u32 k_fence_character = 1 << 8;
constexpr u32 k_fence_length_mask = 0xff;

[[nodiscard]] usize content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize skip_space(std::string_view source, usize position, usize end) noexcept {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] bool name_start(char value) noexcept { return ascii_identifier_start(value) || value == ':'; }

[[nodiscard]] bool name_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == ':' || value == '-' || value == '.';
}

[[nodiscard]] usize entity_end(std::string_view source, usize position, usize end) noexcept {
    ++position;
    while (position < end && (ascii_alphanumeric(source[position]) || source[position] == '#' || source[position] == 'x')) ++position;
    if (position < end && source[position] == ';') ++position;
    return position;
}

[[nodiscard]] usize quoted_end(std::string_view source, usize position, usize end, char quote) noexcept {
    while (position < end && source[position] != quote) ++position;
    return position < end ? position + 1 : position;
}

void paint_entities(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] != '&') {
            ++position;
            continue;
        }
        const usize token_end = entity_end(context.source, position, end);
        paint(context, {.begin = position, .end = token_end}, Style::ENTITY);
        position = token_end;
    }
}

struct TagResult {
    usize position = 0;
    bool closed = false;
};

[[nodiscard]] TagResult lex_tag(LexContext &context, usize position, usize end, bool opening) {
    bool expect_name = opening;
    while (position < end) {
        const char current = context.source[position];
        if (current == ' ' || current == '\t') {
            ++position;
        } else if (context.source.substr(position).starts_with("/>") || context.source.substr(position).starts_with("?>")) {
            paint(context, {.begin = position, .end = position + 2}, Style::OPERATOR);
            return {.position = position + 2, .closed = true};
        } else if (current == '>') {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            return {.position = position + 1, .closed = true};
        } else if (current == '=') {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        } else if (current == '"' || current == '\'') {
            const usize token_begin = position++;
            position = quoted_end(context.source, position, end, current);
            paint(context, {.begin = token_begin, .end = position}, Style::VALUE);
            paint_entities(context, token_begin, position);
        } else if (name_start(current)) {
            const usize token_begin = position++;
            while (position < end && name_continue(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, expect_name ? Style::TAG : Style::ATTRIBUTE);
            expect_name = false;
        } else if (current == '&') {
            const usize token_end = entity_end(context.source, position, end);
            paint(context, {.begin = position, .end = token_end}, Style::ENTITY);
            position = token_end;
        } else {
            paint(context, {.begin = position, .end = position + 1}, Style::OPERATOR);
            ++position;
        }
    }
    return {.position = position};
}

[[nodiscard]] u32 lex_markup(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    while (position < line_end) {
        const u32 kind = state & k_state_mask;
        if (kind == k_comment || kind == k_cdata) {
            const std::string_view delimiter = kind == k_comment ? "-->" : "]]>";
            const usize found = context.source.find(delimiter, position);
            const usize token_end = found == std::string_view::npos || found >= line_end ? line_end : found + delimiter.size();
            paint(context, {.begin = position, .end = token_end}, kind == k_comment ? Style::COMMENT : Style::DOCUMENTATION);
            position = token_end;
            if (found == std::string_view::npos || found >= line_end) break;
            state = k_normal;
            continue;
        }
        if (kind == k_tag) {
            const TagResult result = lex_tag(context, position, line_end, false);
            position = result.position;
            if (!result.closed) break;
            state = k_normal;
            continue;
        }

        if (context.source.substr(position).starts_with("<!--")) {
            state = k_comment;
            continue;
        }
        if (context.source.substr(position).starts_with("<![CDATA[")) {
            state = k_cdata;
            continue;
        }
        if (context.source.substr(position).starts_with("<!") || context.source.substr(position).starts_with("<?")) {
            const usize close = context.source.find(context.source[position + 1] == '?' ? "?>" : ">", position + 2);
            const usize token_end =
                close == std::string_view::npos || close >= line_end ? line_end : close + (context.source[position + 1] == '?' ? 2 : 1);
            paint(context, {.begin = position, .end = token_end}, Style::DIRECTIVE);
            position = token_end;
            if (close == std::string_view::npos || close >= line_end) state = k_tag;
            continue;
        }
        if (context.source[position] == '<' && position + 1 < line_end &&
            (name_start(context.source[position + 1]) ||
             (context.source[position + 1] == '/' && position + 2 < line_end && name_start(context.source[position + 2])))) {
            const usize opener_end = context.source[position + 1] == '/' ? position + 2 : position + 1;
            paint(context, {.begin = position, .end = opener_end}, Style::OPERATOR);
            const TagResult result = lex_tag(context, opener_end, line_end, true);
            position = result.position;
            if (!result.closed) state = k_tag;
            continue;
        }
        if (context.source[position] == '&') {
            const usize token_end = entity_end(context.source, position, line_end);
            paint(context, {.begin = position, .end = token_end}, Style::ENTITY);
            position = token_end;
            continue;
        }
        ++position;
    }

    if (end > line_end && (state & k_state_mask) != k_normal) {
        const Style style = (state & k_state_mask) == k_comment ? Style::COMMENT :
                            (state & k_state_mask) == k_cdata   ? Style::DOCUMENTATION :
                            (state & k_state_mask) == k_tag     ? Style::DEFAULT :
                                                                  Style::DEFAULT;
        paint(context, {.begin = line_end, .end = end}, style);
    }
    if (dialect == Dialect::XML && state == k_normal) paint_entities(context, begin, line_end);
    return state;
}

[[nodiscard]] usize marker_count(std::string_view source, usize position, usize end, char marker) noexcept {
    const usize begin = position;
    while (position < end && source[position] == marker) ++position;
    return position - begin;
}

void lex_markdown_inline(LexContext &context, usize position, usize end) {
    while (position < end) {
        const char current = context.source[position];
        if (current == '`') {
            const usize count = marker_count(context.source, position, end, '`');
            const usize token_begin = position;
            position += count;
            const std::string delimiter(count, '`');
            const usize close = context.source.find(delimiter, position);
            position = close == std::string_view::npos || close >= end ? end : close + count;
            paint(context, {.begin = token_begin, .end = position}, Style::CODE);
        } else if (current == '[') {
            const usize text_close = context.source.find(']', position + 1);
            if (text_close != std::string_view::npos && text_close < end && text_close + 1 < end && context.source[text_close + 1] == '(') {
                const usize target_close = context.source.find(')', text_close + 2);
                const usize token_end = target_close == std::string_view::npos || target_close >= end ? end : target_close + 1;
                paint(context, {.begin = position, .end = text_close + 1}, Style::VALUE);
                paint(context, {.begin = text_close + 1, .end = token_end}, Style::LINK);
                position = token_end;
            } else {
                ++position;
            }
        } else if (current == '<') {
            const usize close = context.source.find('>', position + 1);
            if (close != std::string_view::npos && close < end) {
                paint(context, {.begin = position, .end = close + 1}, Style::LINK);
                position = close + 1;
            } else {
                ++position;
            }
        } else if (current == '&') {
            const usize token_end = entity_end(context.source, position, end);
            paint(context, {.begin = position, .end = token_end}, Style::ENTITY);
            position = token_end;
        } else if (current == '*' || current == '_' || current == '~') {
            const usize count = marker_count(context.source, position, end, current);
            paint(context, {.begin = position, .end = position + count}, Style::OPERATOR);
            position += count;
        } else if (current == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ENTITY);
            position += 2;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] u32 lex_markdown(LexContext &context, usize begin, usize end, u32 state) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = skip_space(context.source, begin, line_end);
    const usize indent = position - begin;
    if ((state & k_state_mask) == k_markdown_fence) {
        const char marker = (state & k_fence_character) != 0 ? '~' : '`';
        const usize required = state & k_fence_length_mask;
        const usize count = marker_count(context.source, position, line_end, marker);
        if (count >= required) {
            paint(context, {.begin = position, .end = line_end}, Style::FENCE);
            return k_normal;
        }
        paint(context, {.begin = begin, .end = end}, Style::CODE);
        return state;
    }
    if (position == line_end) return k_normal;
    if (context.source.substr(position).starts_with("<!--")) return lex_markup(context, begin, end, k_normal, Dialect::HTML);

    if (context.source[position] == '`' || context.source[position] == '~') {
        const char marker = context.source[position];
        const usize count = marker_count(context.source, position, line_end, marker);
        if (count >= 3) {
            paint(context, {.begin = position, .end = position + count}, Style::FENCE);
            if (position + count < line_end) paint(context, {.begin = position + count, .end = line_end}, Style::TAG);
            return k_markdown_fence | static_cast<u32>(std::min<usize>(count, k_fence_length_mask)) |
                   (marker == '~' ? k_fence_character : 0);
        }
    }
    if (indent >= 4) {
        paint(context, {.begin = position, .end = end}, Style::CODE);
        return k_normal;
    }
    if (context.source[position] == '#') {
        const usize count = marker_count(context.source, position, line_end, '#');
        if (count <= 6 && position + count < line_end && ascii_space(context.source[position + count])) {
            paint(context, {.begin = position, .end = line_end}, Style::HEADING);
            return k_normal;
        }
    }
    if (context.source.substr(position).starts_with("---") || context.source.substr(position).starts_with("***") ||
        context.source.substr(position).starts_with("___")) {
        paint(context, {.begin = position, .end = line_end}, Style::OPERATOR);
        return k_normal;
    }
    if (std::string_view(">-+*").contains(context.source[position])) {
        const usize marker_end = position + 1 < line_end && ascii_space(context.source[position + 1]) ? position + 1 : position;
        if (marker_end != position) paint(context, {.begin = position, .end = marker_end}, Style::OPERATOR);
    }
    lex_markdown_inline(context, position, line_end);
    return k_normal;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    paint(context, {.begin = begin, .end = end}, Style::DEFAULT);
    return dialect == Dialect::MARKDOWN ? lex_markdown(context, begin, end, state) : lex_markup(context, begin, end, state, dialect);
}

} // namespace

void MarkupLexer::lex(LexContext &context) const {
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
