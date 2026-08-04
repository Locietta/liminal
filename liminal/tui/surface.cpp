#include "surface.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <lighter/encoding/utf8.h>

namespace liminal::tui {

namespace {

bool in_range(char32_t value, char32_t first, char32_t last) noexcept { return value >= first && value <= last; }

bool terminal_control(char32_t value) noexcept { return value < 0x20 || in_range(value, 0x7f, 0x9f); }

bool combining(char32_t value) noexcept {
    return in_range(value, 0x0300, 0x036f) || in_range(value, 0x1ab0, 0x1aff) || in_range(value, 0x1dc0, 0x1dff) ||
           in_range(value, 0x20d0, 0x20ff) || in_range(value, 0xfe00, 0xfe0f) || in_range(value, 0xfe20, 0xfe2f) || value == 0x200d;
}

bool wide(char32_t value) noexcept {
    return in_range(value, 0x1100, 0x115f) || value == 0x2329 || value == 0x232a || in_range(value, 0x2e80, 0xa4cf) ||
           in_range(value, 0xac00, 0xd7a3) || in_range(value, 0xf900, 0xfaff) || in_range(value, 0xfe10, 0xfe19) ||
           in_range(value, 0xfe30, 0xfe6f) || in_range(value, 0xff00, 0xff60) || in_range(value, 0xffe0, 0xffe6) ||
           in_range(value, 0x1f300, 0x1faff) || in_range(value, 0x20000, 0x3fffd);
}

std::string_view style_sequence(Style style) {
    switch (style) {
        case Style::NORMAL: return "\x1b[0m";
        case Style::EMPHASIS: return "\x1b[1m";
        case Style::MUTED: return "\x1b[2m";
        case Style::ACCENT: return "\x1b[36m";
        case Style::FAILURE: return "\x1b[31m";
    }
    return "\x1b[0m";
}

void append_safe_cell_text(std::string &output, std::string_view text) { output += sanitize_terminal_text(text); }

} // namespace

Surface::Surface(i32 columns, i32 rows)
    : columns(std::max(columns, 0)), rows(std::max(rows, 0)), cells(static_cast<usize>(this->columns * this->rows)) {}

void Surface::clear() { std::ranges::fill(cells, Cell{}); }

i32 Surface::write(i32 row, i32 column, std::string_view text, Style style) {
    if (row < 0 || row >= rows || column >= columns) return column;
    i32 current = std::max(column, 0);
    usize offset = 0;
    while (offset < text.size()) {
        auto decoded = lighter::encoding::utf8::decode_one(text.substr(offset));
        auto encoded = decoded.status == lighter::encoding::utf8::DecodeStatus::OK ? text.substr(offset, decoded.size) :
                                                                                     lighter::encoding::utf8::k_replacement;
        offset += decoded.size;
        if (terminal_control(decoded.codepoint)) {
            decoded.codepoint = 0xfffd;
            encoded = lighter::encoding::utf8::k_replacement;
        }
        const auto width = cell_width(decoded.codepoint);
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
        auto &cell = cells[static_cast<usize>(row * columns + current)];
        cell = {.text = std::string(encoded), .style = style};
        if (width == 2) {
            cells[static_cast<usize>(row * columns + current + 1)] = {.text = {}, .style = style, .continuation = true};
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
    if (combining(codepoint)) return 0;
    return wide(codepoint) ? 2 : 1;
}

i32 text_width(std::string_view text) noexcept {
    i32 width = 0;
    usize offset = 0;
    while (offset < text.size()) {
        const auto decoded = lighter::encoding::utf8::decode_one(text.substr(offset));
        offset += decoded.size;
        width += cell_width(decoded.codepoint);
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
    Style active = Style::NORMAL;
    for (i32 row = 0; row < frame.surface.rows; ++row) {
        output += "\x1b[2K";
        for (i32 column = 0; column < frame.surface.columns; ++column) {
            const auto &cell = frame.surface.cells[static_cast<usize>(row * frame.surface.columns + column)];
            if (cell.continuation) continue;
            if (cell.style != active) {
                output += style_sequence(cell.style);
                active = cell.style;
            }
            append_safe_cell_text(output, cell.text);
        }
        if (row + 1 < frame.surface.rows) output += "\r\n";
    }
    output += "\x1b[0m";
    if (frame.cursor.visible && frame.surface.rows > 0 && frame.surface.columns > 0) {
        const auto row = std::clamp(frame.cursor.row, 0, frame.surface.rows - 1);
        const auto column = std::clamp(frame.cursor.column, 0, frame.surface.columns - 1);
        output += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H\x1b[?25h";
    }
    return output;
}

} // namespace liminal::tui
