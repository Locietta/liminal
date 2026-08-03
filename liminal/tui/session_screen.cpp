#include "session_screen.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/encoding/utf8.h>

namespace liminal::tui {

namespace {

struct LayoutRow {
    u64 block_id = 0;
    usize source_offset = 0;
    std::string text;
    Style style = Style::NORMAL;
};

usize previous_boundary(std::string_view text, usize cursor) {
    if (cursor == 0) return 0;
    auto previous = cursor - 1;
    while (previous > 0 && (static_cast<unsigned char>(text[previous]) & 0xc0) == 0x80) --previous;
    return previous;
}

usize next_boundary(std::string_view text, usize cursor) {
    if (cursor >= text.size()) return text.size();
    const auto decoded = lighter::encoding::utf8::decode_one(text.substr(cursor));
    return std::min(text.size(), cursor + decoded.size);
}

Style block_style(const Block &block) {
    if (block.state == BlockState::FAILED) return Style::FAILURE;
    switch (block.kind) {
        case BlockKind::USER: return Style::ACCENT;
        case BlockKind::TOOL: return Style::MUTED;
        case BlockKind::NOTICE: return block.state == BlockState::CANCELLED ? Style::FAILURE : Style::MUTED;
        case BlockKind::ASSISTANT: return Style::NORMAL;
    }
    return Style::NORMAL;
}

std::string block_text(const Block &block) {
    switch (block.kind) {
        case BlockKind::USER: return "you: " + block.text;
        case BlockKind::ASSISTANT: return "assistant: " + block.text;
        case BlockKind::TOOL: {
            auto state = std::string("completed");
            if (block.state == BlockState::RUNNING) state = "running";
            if (block.state == BlockState::CANCELLED) state = "cancelled";
            if (block.state == BlockState::FAILED) state = "failed";
            return "[tool: " + block.text + " - " + state + "]";
        }
        case BlockKind::NOTICE: return block.text;
    }
    return block.text;
}

std::vector<LayoutRow> wrap_block(const Block &block, i32 columns) {
    std::vector<LayoutRow> rows;
    const auto source = block_text(block);
    const auto width = std::max(columns, 1);
    LayoutRow current{.block_id = block.id, .style = block_style(block)};
    i32 used = 0;
    usize offset = 0;
    while (offset < source.size()) {
        const auto start = offset;
        const auto decoded = lighter::encoding::utf8::decode_one(std::string_view(source).substr(offset));
        const auto encoded = decoded.status == lighter::encoding::utf8::DecodeStatus::OK ?
                                 std::string_view(source).substr(offset, decoded.size) :
                                 lighter::encoding::utf8::k_replacement;
        offset += decoded.size;
        if (decoded.codepoint == '\r') continue;
        if (decoded.codepoint == '\n') {
            rows.push_back(std::move(current));
            current = {.block_id = block.id, .source_offset = offset, .style = block_style(block)};
            used = 0;
            continue;
        }
        const auto cells = std::max(cell_width(decoded.codepoint), 0);
        if (cells > 0 && used > 0 && used + cells > width) {
            rows.push_back(std::move(current));
            current = {.block_id = block.id, .source_offset = start, .style = block_style(block)};
            used = 0;
        }
        current.text += encoded;
        used += cells;
    }
    rows.push_back(std::move(current));
    return rows;
}

std::vector<LayoutRow> layout_transcript(const Transcript &transcript, i32 columns) {
    std::vector<LayoutRow> rows;
    for (const auto &block : transcript.blocks) {
        auto block_rows = wrap_block(block, columns);
        rows.insert(rows.end(), std::make_move_iterator(block_rows.begin()), std::make_move_iterator(block_rows.end()));
    }
    return rows;
}

usize find_anchor(const std::vector<LayoutRow> &rows, const ViewportAnchor &anchor) {
    usize closest = rows.size();
    for (usize index = 0; index < rows.size(); ++index) {
        if (rows[index].block_id != anchor.block_id) continue;
        if (rows[index].source_offset > anchor.source_offset) break;
        closest = index;
    }
    return closest == rows.size() ? 0 : closest;
}

usize viewport_start(const SessionScreen &screen, const std::vector<LayoutRow> &rows) {
    const auto visible = static_cast<usize>(std::max(screen.viewport_rows(), 0));
    const auto tail = rows.size() > visible ? rows.size() - visible : 0;
    if (!screen.anchor) return tail;
    return std::min(find_anchor(rows, *screen.anchor), tail);
}

struct DisplayComposer {
    std::string text;
    i32 cursor_column = 0;
};

DisplayComposer display_composer(const Composer &composer) {
    DisplayComposer display;
    usize offset = 0;
    while (offset < composer.text.size()) {
        if (offset == composer.cursor) display.cursor_column = text_width(display.text);
        const auto decoded = lighter::encoding::utf8::decode_one(std::string_view(composer.text).substr(offset));
        const auto encoded = decoded.status == lighter::encoding::utf8::DecodeStatus::OK ?
                                 std::string_view(composer.text).substr(offset, decoded.size) :
                                 lighter::encoding::utf8::k_replacement;
        offset += decoded.size;
        if (decoded.codepoint == '\r') continue;
        if (decoded.codepoint == '\n') {
            display.text += "\\n";
        } else if (decoded.codepoint == '\t') {
            display.text += "\\t";
        } else {
            display.text += encoded;
        }
    }
    if (composer.cursor == composer.text.size()) display.cursor_column = text_width(display.text);
    return display;
}

std::string cell_slice(std::string_view text, i32 first, i32 width) {
    std::string output;
    i32 column = 0;
    usize offset = 0;
    while (offset < text.size() && text_width(output) < width) {
        const auto decoded = lighter::encoding::utf8::decode_one(text.substr(offset));
        const auto encoded = decoded.status == lighter::encoding::utf8::DecodeStatus::OK ? text.substr(offset, decoded.size) :
                                                                                           lighter::encoding::utf8::k_replacement;
        offset += decoded.size;
        const auto cells = cell_width(decoded.codepoint);
        if (cells == 0) {
            if (column >= first && !output.empty()) output += encoded;
            continue;
        }
        if (column + cells <= first) {
            column += cells;
            continue;
        }
        if (column < first || text_width(output) + cells > width) break;
        output += encoded;
        column += cells;
    }
    return output;
}

std::string trim_notice(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

} // namespace

void Composer::insert(std::string_view value) {
    text.insert(cursor, value);
    cursor += value.size();
}

void Composer::backspace() {
    const auto previous = previous_boundary(text, cursor);
    text.erase(previous, cursor - previous);
    cursor = previous;
}

void Composer::erase() {
    const auto next = next_boundary(text, cursor);
    text.erase(cursor, next - cursor);
}

void Composer::move_left() { cursor = previous_boundary(text, cursor); }

void Composer::move_right() { cursor = next_boundary(text, cursor); }

void Composer::move_home() noexcept { cursor = 0; }

void Composer::move_end() noexcept { cursor = text.size(); }

std::string Composer::take() {
    cursor = 0;
    return std::exchange(text, {});
}

void SessionScreen::resize(lighter::TerminalSize next) noexcept {
    size.columns = std::max(next.columns, 1);
    size.rows = std::max(next.rows, 1);
}

void SessionScreen::set_model(std::string_view name, const std::optional<std::string> &next_effort) {
    model = name;
    effort = next_effort;
}

void SessionScreen::apply(const Event &event) {
    transcript.apply(event);
    if (anchor) unread = true;
}

void SessionScreen::add_notice(std::string text) { apply(SessionNotice{.text = trim_notice(std::move(text))}); }

void SessionScreen::scroll(i32 row_delta) {
    const auto rows = layout_transcript(transcript, size.columns);
    if (rows.empty() || viewport_rows() <= 0) return;
    const auto visible = static_cast<usize>(viewport_rows());
    const auto tail = rows.size() > visible ? rows.size() - visible : 0;
    const auto current = viewport_start(*this, rows);
    const auto target = static_cast<usize>(std::clamp<i64>(static_cast<i64>(current) + row_delta, 0, static_cast<i64>(tail)));
    if (target == tail) {
        follow_tail();
        return;
    }
    anchor = ViewportAnchor{.block_id = rows[target].block_id, .source_offset = rows[target].source_offset};
}

void SessionScreen::page(i32 direction) { scroll(direction * std::max(viewport_rows() - 1, 1)); }

void SessionScreen::follow_tail() noexcept {
    anchor.reset();
    unread = false;
}

i32 SessionScreen::viewport_rows() const noexcept { return std::max(size.rows - 3, 0); }

Frame SessionScreen::frame() const {
    Frame result{.surface = Surface(size.columns, size.rows)};
    if (size.rows <= 0 || size.columns <= 0) return result;

    const bool header = size.rows >= 2;
    const bool status = size.rows >= 3;
    const auto prompt_row = size.rows - 1;
    if (header) {
        auto selection = model;
        if (effort) selection += "@" + *effort;
        result.surface.write(0, 0, "liminal  " + selection, Style::EMPHASIS);
    }

    const auto rows = layout_transcript(transcript, size.columns);
    const auto start = viewport_start(*this, rows);
    const auto visible = static_cast<usize>(std::max(viewport_rows(), 0));
    for (usize index = 0; index < visible && start + index < rows.size(); ++index) {
        const auto &row = rows[start + index];
        result.surface.write(static_cast<i32>(index) + (header ? 1 : 0), 0, row.text, row.style);
    }

    if (status) {
        std::string status_text;
        if (anchor) {
            status_text = "history";
            if (unread) status_text += " | new output";
        } else {
            status_text = "PageUp/PageDown scroll";
        }
        result.surface.write(size.rows - 2, 0, status_text, Style::MUTED);
    }

    auto displayed = display_composer(composer);
    auto prefix = model;
    if (effort) prefix += "@" + *effort;
    prefix += " > ";
    const auto cursor = text_width(prefix) + displayed.cursor_column;
    const auto first = std::max(cursor - size.columns + 1, 0);
    const auto line = cell_slice(prefix + displayed.text, first, size.columns);
    result.surface.write(prompt_row, 0, line, Style::NORMAL);
    result.cursor = {.row = prompt_row, .column = std::clamp(cursor - first, 0, size.columns - 1), .visible = true};
    return result;
}

} // namespace liminal::tui
