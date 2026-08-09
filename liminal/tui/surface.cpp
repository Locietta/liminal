#include "surface.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/width.h>

#include <lighter/encoding/utf8.h>

namespace liminal::tui {

namespace {

bool in_range(char32_t value, char32_t first, char32_t last) noexcept { return value >= first && value <= last; }

bool terminal_control(char32_t value) noexcept { return value < 0x20 || in_range(value, 0x7f, 0x9f); }

std::string_view style_sequence(Style style) {
    switch (style) {
        case Style::NORMAL: return "\x1b[0m";
        case Style::EMPHASIS: return "\x1b[1m";
        case Style::ITALIC: return "\x1b[3m";
        case Style::MUTED: return "\x1b[2m";
        case Style::ACCENT: return "\x1b[36m";
        case Style::CODE: return "\x1b[22;38;2;255;229;154m";
        case Style::CODE_KEYWORD: return "\x1b[1;38;2;240;200;255m";
        case Style::CODE_PREPROCESSOR: return "\x1b[1;38;2;255;210;138m";
        case Style::CODE_TYPE: return "\x1b[22;38;2;154;239;255m";
        case Style::CODE_FUNCTION: return "\x1b[22;38;2;169;204;255m";
        case Style::CODE_STRING: return "\x1b[22;38;2;183;244;173m";
        case Style::CODE_COMMENT: return "\x1b[22;38;2;212;218;234m";
        case Style::CODE_NUMBER: return "\x1b[22;38;2;255;229;154m";
        case Style::CODE_CONSTANT: return "\x1b[22;38;2;165;242;226m";
        case Style::CODE_PROPERTY: return "\x1b[22;38;2;255;208;240m";
        case Style::CODE_OPERATOR: return "\x1b[22;38;2;240;200;255m";
        case Style::LINK: return "\x1b[4;36m";
        case Style::DIFF_ADDITION: return "\x1b[32m";
        case Style::DIFF_DELETION: return "\x1b[31m";
        case Style::DIFF_HUNK: return "\x1b[1;36m";
        case Style::FAILURE: return "\x1b[31m";
    }
    return "\x1b[0m";
}

void append_safe_cell_text(std::string &output, std::string_view text) { output += sanitize_terminal_text(text); }

void append_row(std::string &output, const Surface &surface, i32 row) {
    Style active = Style::NORMAL;
    output += "\x1b[0m\x1b[2K";
    for (i32 column = 0; column < surface.columns; ++column) {
        const auto &cell = surface.cells[static_cast<usize>(row * surface.columns + column)];
        if (cell.continuation) continue;
        if (cell.style != active) {
            output += style_sequence(cell.style);
            active = cell.style;
        }
        append_safe_cell_text(output, cell.text);
    }
}

bool row_equal(const Surface &left, const Surface &right, i32 row) {
    const auto offset = static_cast<usize>(row * left.columns);
    return std::equal(left.cells.begin() + static_cast<isize>(offset),
                      left.cells.begin() + static_cast<isize>(offset + static_cast<usize>(left.columns)),
                      right.cells.begin() + static_cast<isize>(offset));
}

void append_cursor(std::string &output, const Frame &frame) {
    output += "\x1b[0m";
    if (frame.cursor.visible && frame.surface.rows > 0 && frame.surface.columns > 0) {
        const auto row = std::clamp(frame.cursor.row, 0, frame.surface.rows - 1);
        const auto column = std::clamp(frame.cursor.column, 0, frame.surface.columns - 1);
        output += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H\x1b[?25h";
    }
}

} // namespace

Surface::Surface(i32 columns, i32 rows)
    : columns(std::max(columns, 0)), rows(std::max(rows, 0)), cells(static_cast<usize>(this->columns * this->rows)) {}

void Surface::clear() { std::ranges::fill(cells, Cell{}); }

i32 Surface::write(i32 row, i32 column, std::string_view text, Style style) {
    if (row < 0 || row >= rows || column >= columns) return column;
    const auto clear_cluster = [this, row](i32 target) {
        if (target < 0 || target >= columns) return;
        auto owner = target;
        while (owner > 0 && cells[static_cast<usize>(row * columns + owner)].continuation) --owner;
        cells[static_cast<usize>(row * columns + owner)] = {};
        for (auto continuation = owner + 1; continuation < columns && cells[static_cast<usize>(row * columns + continuation)].continuation;
             ++continuation) {
            cells[static_cast<usize>(row * columns + continuation)] = {};
        }
    };
    i32 current = std::max(column, 0);
    usize offset = 0;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        auto encoded = sanitize_terminal_text(text.substr(grapheme.offset, grapheme.size));
        offset += grapheme.size;
        const auto width = grapheme.width;
        if (width == 0) {
            if (current > 0 && current <= columns) {
                auto previous = current - 1;
                while (previous > 0 && cells[static_cast<usize>(row * columns + previous)].continuation) --previous;
                cells[static_cast<usize>(row * columns + previous)].text += encoded;
            }
            continue;
        }
        if (current < 0) {
            current += width;
            continue;
        }
        if (current + width > columns) break;
        for (i32 target = current; target < current + width; ++target) clear_cluster(target);
        auto &cell = cells[static_cast<usize>(row * columns + current)];
        cell = {.text = std::move(encoded), .style = style};
        for (i32 continuation = 1; continuation < width; ++continuation) {
            cells[static_cast<usize>(row * columns + current + continuation)] = {.text = {}, .style = style, .continuation = true};
        }
        current += width;
    }
    return current;
}

std::string Surface::row_text(i32 row) const {
    if (row < 0 || row >= rows) return {};
    std::string text;
    for (i32 column = 0; column < columns; ++column) {
        const auto &cell = cells[static_cast<usize>(row * columns + column)];
        if (!cell.continuation) text += cell.text;
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

i32 cell_width(char32_t codepoint) noexcept {
    if (terminal_control(codepoint)) return 1;
    return static_cast<i32>(unicode::width(codepoint));
}

GraphemeSpan next_grapheme(std::string_view text, usize offset) noexcept {
    offset = std::min(offset, text.size());
    if (offset == text.size()) return {.offset = offset};

    const auto first = lighter::encoding::utf8::decode_one(text.substr(offset));
    if (first.status != lighter::encoding::utf8::DecodeStatus::OK || terminal_control(first.codepoint)) {
        return {.offset = offset, .size = first.size, .width = 1};
    }

    unicode::grapheme_segmenter_state state;
    unicode::grapheme_process_init(first.codepoint, state);
    unicode::grapheme_cluster_width_accumulator width;
    width.push(first.codepoint);

    auto end = offset + first.size;
    while (end < text.size()) {
        const auto decoded = lighter::encoding::utf8::decode_one(text.substr(end));
        if (decoded.status != lighter::encoding::utf8::DecodeStatus::OK || terminal_control(decoded.codepoint) ||
            unicode::grapheme_process_breakable(decoded.codepoint, state)) {
            break;
        }
        width.push(decoded.codepoint);
        end += decoded.size;
    }
    return {.offset = offset, .size = end - offset, .width = static_cast<i32>(width.width())};
}

usize previous_grapheme_boundary(std::string_view text, usize offset) noexcept {
    const auto target = std::min(offset, text.size());
    usize current = 0;
    usize previous = 0;
    while (current < target) {
        previous = current;
        const auto grapheme = next_grapheme(text, current);
        current += grapheme.size;
    }
    return previous;
}

usize next_grapheme_boundary(std::string_view text, usize offset) noexcept {
    const auto start = std::min(offset, text.size());
    if (start == text.size()) return start;
    const auto grapheme = next_grapheme(text, start);
    return start + grapheme.size;
}

i32 text_width(std::string_view text) noexcept {
    i32 width = 0;
    usize offset = 0;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        offset += grapheme.size;
        width += grapheme.width;
    }
    return width;
}

std::string sanitize_terminal_text(std::string_view text, bool preserve_layout_controls) {
    std::string output;
    output.reserve(text.size());
    usize offset = 0;
    while (offset < text.size()) {
        const auto decoded = lighter::encoding::utf8::decode_one(text.substr(offset));
        const auto encoded = decoded.status == lighter::encoding::utf8::DecodeStatus::OK ? text.substr(offset, decoded.size) :
                                                                                           lighter::encoding::utf8::k_replacement;
        offset += decoded.size;
        const bool layout_control = decoded.codepoint == '\n' || decoded.codepoint == '\r' || decoded.codepoint == '\t';
        output += terminal_control(decoded.codepoint) && !(preserve_layout_controls && layout_control) ?
                      lighter::encoding::utf8::k_replacement :
                      encoded;
    }
    return output;
}

std::string encode_frame(const Frame &frame) {
    std::string output = "\x1b[?25l\x1b[H";
    for (i32 row = 0; row < frame.surface.rows; ++row) {
        append_row(output, frame.surface, row);
        if (row + 1 < frame.surface.rows) output += "\r\n";
    }
    append_cursor(output, frame);
    return output;
}

std::string encode_frame_diff(const Frame *previous, const Frame &frame) {
    if (!previous || previous->surface.columns != frame.surface.columns || previous->surface.rows != frame.surface.rows) {
        return encode_frame(frame);
    }

    std::string output;
    for (i32 row = 0; row < frame.surface.rows; ++row) {
        if (row_equal(previous->surface, frame.surface, row)) continue;
        if (output.empty()) output += "\x1b[?25l";
        output += "\x1b[" + std::to_string(row + 1) + ";1H";
        append_row(output, frame.surface, row);
    }
    if (previous->cursor != frame.cursor) {
        if (output.empty()) output += "\x1b[?25l";
    }
    if (!output.empty()) append_cursor(output, frame);
    return output;
}

} // namespace liminal::tui
