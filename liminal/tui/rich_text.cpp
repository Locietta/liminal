#include "rich_text.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace liminal::tui {

namespace {

struct LogicalLine {
    usize source_offset = 0;
    usize source_size = 0;
    std::vector<StyledSpan> spans;
    std::vector<StyledSpan> continuation;
    bool preserve_whitespace = false;
};

void append_span(std::vector<StyledSpan> &spans, std::string_view text, Style style) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
        spans.back().text += text;
    } else {
        spans.push_back({.text = std::string(text), .style = style});
    }
}

std::string_view trim_left(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    return text;
}

std::string_view trim(std::string_view text) {
    text = trim_left(text);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool word_byte(char value) { return std::isalnum(static_cast<unsigned char>(value)) != 0; }

std::vector<StyledSpan> parse_inline(std::string_view text, Style base) {
    std::vector<StyledSpan> spans;
    usize offset = 0;
    while (offset < text.size()) {
        if (text[offset] == '\\' && offset + 1 < text.size()) {
            append_span(spans, text.substr(offset + 1, 1), base);
            offset += 2;
            continue;
        }
        if (text[offset] == '[') {
            const auto label_end = text.find(']', offset + 1);
            if (label_end != std::string_view::npos && label_end + 1 < text.size() && text[label_end + 1] == '(') {
                const auto target_end = text.find(')', label_end + 2);
                if (target_end != std::string_view::npos && label_end > offset + 1 && target_end > label_end + 2) {
                    append_span(spans, text.substr(offset + 1, label_end - offset - 1), Style::LINK);
                    append_span(spans, " <", Style::LINK);
                    append_span(spans, text.substr(label_end + 2, target_end - label_end - 2), Style::LINK);
                    append_span(spans, ">", Style::LINK);
                    offset = target_end + 1;
                    continue;
                }
            }
        }
        if (text[offset] == '`') {
            const auto end = text.find('`', offset + 1);
            if (end != std::string_view::npos && end > offset + 1) {
                append_span(spans, text.substr(offset + 1, end - offset - 1), Style::CODE);
                offset = end + 1;
                continue;
            }
        }
        const bool underscore_word = text[offset] == '_' && offset > 0 && word_byte(text[offset - 1]);
        const bool strong = !underscore_word && offset + 1 < text.size() &&
                            ((text[offset] == '*' && text[offset + 1] == '*') || (text[offset] == '_' && text[offset + 1] == '_'));
        if (strong) {
            const auto delimiter = text.substr(offset, 2);
            const auto end = text.find(delimiter, offset + 2);
            const bool closes_inside_word =
                delimiter == "__" && end != std::string_view::npos && end + 2 < text.size() && word_byte(text[end + 2]);
            if (end != std::string_view::npos && end > offset + 2 && !closes_inside_word) {
                append_span(spans, text.substr(offset + 2, end - offset - 2), Style::EMPHASIS);
                offset = end + 2;
                continue;
            }
        }
        if (text[offset] == '*' || (text[offset] == '_' && !underscore_word)) {
            const auto end = text.find(text[offset], offset + 1);
            const bool closes_inside_word =
                text[offset] == '_' && end != std::string_view::npos && end + 1 < text.size() && word_byte(text[end + 1]);
            if (end != std::string_view::npos && end > offset + 1 && !closes_inside_word) {
                append_span(spans, text.substr(offset + 1, end - offset - 1), Style::ITALIC);
                offset = end + 1;
                continue;
            }
        }

        auto end = offset + 1;
        while (end < text.size() && text[end] != '\\' && text[end] != '[' && text[end] != '`' && text[end] != '*' && text[end] != '_') {
            ++end;
        }
        append_span(spans, text.substr(offset, end - offset), base);
        offset = end;
    }
    return spans;
}

Style diff_style(std::string_view line, bool fenced_diff) {
    if (line.starts_with("@@")) return Style::DIFF_HUNK;
    if (line.starts_with("diff --git ") || line.starts_with("index ") || line.starts_with("--- ") || line.starts_with("+++ ")) {
        return Style::DIFF_HUNK;
    }
    if (line.starts_with('+')) return Style::DIFF_ADDITION;
    if (line.starts_with('-')) return Style::DIFF_DELETION;
    return fenced_diff ? Style::CODE : Style::NORMAL;
}

bool looks_like_diff(std::string_view line) {
    return line.starts_with("diff --git ") || line.starts_with("index ") || line.starts_with("--- ") || line.starts_with("+++ ") ||
           line.starts_with("@@") || (line.starts_with('+') && !line.starts_with("+ ")) ||
           (line.starts_with('-') && !line.starts_with("- "));
}

std::optional<usize> ordered_prefix(std::string_view line) {
    usize offset = 0;
    while (offset < line.size() && std::isdigit(static_cast<unsigned char>(line[offset])) != 0) ++offset;
    if (offset == 0 || offset + 1 >= line.size() || line[offset] != '.' || line[offset + 1] != ' ') return std::nullopt;
    return offset + 2;
}

LogicalLine prose_line(std::string_view line, usize source_offset) {
    LogicalLine result{.source_offset = source_offset, .source_size = line.size()};
    auto content = trim_left(line);
    usize heading = 0;
    while (heading < content.size() && heading < 6 && content[heading] == '#') ++heading;
    if (heading > 0 && heading < content.size() && content[heading] == ' ') {
        result.spans = parse_inline(trim(content.substr(heading + 1)), Style::EMPHASIS);
        return result;
    }

    if (content.size() >= 2 && (content.starts_with("- ") || content.starts_with("* ") || content.starts_with("+ "))) {
        append_span(result.spans, "• ", Style::MUTED);
        auto body = parse_inline(content.substr(2), Style::NORMAL);
        result.spans.insert(result.spans.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
        result.continuation.push_back({.text = "  ", .style = Style::MUTED});
        return result;
    }
    if (const auto prefix = ordered_prefix(content)) {
        append_span(result.spans, content.substr(0, *prefix), Style::MUTED);
        auto body = parse_inline(content.substr(*prefix), Style::NORMAL);
        result.spans.insert(result.spans.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
        result.continuation.push_back({.text = "  ", .style = Style::MUTED});
        return result;
    }
    if (looks_like_diff(content)) {
        append_span(result.spans, content, diff_style(content, false));
        result.continuation.push_back({.text = "  ", .style = Style::MUTED});
        result.preserve_whitespace = true;
        return result;
    }
    result.spans = parse_inline(trim(line), Style::NORMAL);
    return result;
}

i32 spans_width(const std::vector<StyledSpan> &spans) {
    i32 result = 0;
    for (const auto &span : spans) result += text_width(span.text);
    return result;
}

void append_grapheme(std::vector<StyledSpan> &spans, std::string_view grapheme, Style style) { append_span(spans, grapheme, style); }

std::vector<StyledSpan> usable_continuation(const LogicalLine &line, i32 columns) {
    return spans_width(line.continuation) < columns ? line.continuation : std::vector<StyledSpan>{};
}

void push_row(std::vector<StyledRow> &rows, const LogicalLine &line, std::vector<StyledSpan> &spans) {
    rows.push_back({.source_offset = line.source_offset, .spans = std::move(spans)});
    spans.clear();
}

std::vector<StyledRow> wrap_preserved(const LogicalLine &line, i32 columns) {
    std::vector<StyledRow> rows;
    std::vector<StyledSpan> current;
    i32 used = 0;
    const auto continuation = usable_continuation(line, columns);
    auto next_row = [&] {
        push_row(rows, line, current);
        current = continuation;
        used = spans_width(current);
    };

    for (const auto &span : line.spans) {
        usize offset = 0;
        while (offset < span.text.size()) {
            if (span.text[offset] == '\t') {
                for (i32 space = 0; space < 4; ++space) {
                    if (used > 0 && used + 1 > columns) next_row();
                    append_grapheme(current, " ", span.style);
                    ++used;
                }
                ++offset;
                continue;
            }
            const auto grapheme = next_grapheme(span.text, offset);
            const auto encoded = std::string_view(span.text).substr(offset, grapheme.size);
            offset += grapheme.size;
            if (grapheme.width > 0 && used > 0 && used + grapheme.width > columns) next_row();
            append_grapheme(current, encoded, span.style);
            used += std::max(grapheme.width, 0);
        }
    }
    push_row(rows, line, current);
    return rows;
}

std::vector<StyledRow> wrap_prose(const LogicalLine &line, i32 columns) {
    struct Glyph {
        std::string text;
        Style style = Style::NORMAL;
        i32 width = 0;
        bool whitespace = false;
    };

    std::vector<Glyph> glyphs;
    for (const auto &span : line.spans) {
        usize offset = 0;
        while (offset < span.text.size()) {
            const auto grapheme = next_grapheme(span.text, offset);
            const auto encoded = std::string_view(span.text).substr(offset, grapheme.size);
            offset += grapheme.size;
            glyphs.push_back({
                .text = std::string(encoded),
                .style = span.style,
                .width = std::max(grapheme.width, 0),
                .whitespace = encoded == " " || encoded == "\t",
            });
        }
    }

    std::vector<StyledRow> rows;
    std::vector<StyledSpan> current;
    i32 used = 0;
    bool pending_space = false;
    Style space_style = Style::NORMAL;
    const auto continuation = usable_continuation(line, columns);
    const auto continuation_width = spans_width(continuation);
    auto next_row = [&] {
        push_row(rows, line, current);
        current = continuation;
        used = spans_width(current);
        pending_space = false;
    };

    usize offset = 0;
    while (offset < glyphs.size()) {
        if (glyphs[offset].whitespace) {
            pending_space = true;
            space_style = glyphs[offset].style;
            ++offset;
            continue;
        }

        auto word_end = offset;
        i32 word_width = 0;
        while (word_end < glyphs.size() && !glyphs[word_end].whitespace) {
            word_width += glyphs[word_end].width;
            ++word_end;
        }
        const auto separator = pending_space && used > 0 ? 1 : 0;
        const auto row_capacity = columns - continuation_width;
        if (used > 0 && word_width <= row_capacity && used + separator + word_width > columns) next_row();
        if (pending_space && used > 0) {
            if (used + 1 > columns) {
                next_row();
            } else {
                append_grapheme(current, " ", space_style);
                ++used;
            }
        }
        pending_space = false;

        while (offset < word_end) {
            const auto &glyph = glyphs[offset++];
            if (glyph.width > 0 && used > 0 && used + glyph.width > columns) next_row();
            append_grapheme(current, glyph.text, glyph.style);
            used += glyph.width;
        }
    }
    push_row(rows, line, current);
    return rows;
}

} // namespace

std::vector<StyledRow> layout_rich_text(std::string_view source, i32 columns) {
    std::vector<LogicalLine> lines;
    bool fenced = false;
    bool fenced_diff = false;
    std::string fence;
    usize offset = 0;
    while (true) {
        const auto end = source.find('\n', offset);
        auto line = source.substr(offset, end == std::string_view::npos ? source.size() - offset : end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto content = trim_left(line);

        if (!fenced && (content.starts_with("```") || content.starts_with("~~~"))) {
            fence = std::string(content.substr(0, 3));
            const auto language = trim(content.substr(3));
            fenced = true;
            fenced_diff = language == "diff" || language == "patch";
            LogicalLine label{.source_offset = offset, .source_size = line.size()};
            append_span(label.spans, language.empty() ? "[code]" : "[code: " + std::string(language) + "]", Style::MUTED);
            lines.push_back(std::move(label));
        } else if (fenced && content.starts_with(fence)) {
            fenced = false;
            fenced_diff = false;
            fence.clear();
        } else if (fenced) {
            LogicalLine code{.source_offset = offset, .source_size = line.size(), .preserve_whitespace = true};
            append_span(code.spans, line, diff_style(line, fenced_diff));
            code.continuation.push_back({.text = "  ", .style = Style::MUTED});
            lines.push_back(std::move(code));
        } else {
            lines.push_back(prose_line(line, offset));
        }

        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    if (lines.empty()) lines.push_back({});
    lines.front().spans.insert(lines.front().spans.begin(), {.text = "assistant: ", .style = Style::MUTED});

    std::vector<StyledRow> rows;
    const auto width = std::max(columns, 1);
    for (const auto &line : lines) {
        auto wrapped = line.preserve_whitespace ? wrap_preserved(line, width) : wrap_prose(line, width);
        usize visible_offset = 0;
        for (auto &row : wrapped) {
            row.source_offset = line.source_offset + std::min(visible_offset, line.source_size);
            for (const auto &span : row.spans) visible_offset += span.text.size();
        }
        rows.insert(rows.end(), std::make_move_iterator(wrapped.begin()), std::make_move_iterator(wrapped.end()));
    }
    return rows;
}

} // namespace liminal::tui
