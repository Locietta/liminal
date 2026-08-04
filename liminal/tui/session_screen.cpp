#include "session_screen.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace liminal::tui {

namespace {

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
        const auto grapheme = next_grapheme(source, offset);
        const auto encoded = std::string_view(source).substr(offset, grapheme.size);
        offset += grapheme.size;
        if (encoded == "\r") continue;
        if (encoded == "\n") {
            rows.push_back(std::move(current));
            current = {.block_id = block.id, .source_offset = offset, .style = block_style(block)};
            used = 0;
            continue;
        }
        const auto cells = std::max(grapheme.width, 0);
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

std::optional<usize> find_block(const Transcript &transcript, u64 id) {
    if (id > 0 && id <= transcript.blocks.size()) {
        const auto index = static_cast<usize>(id - 1);
        if (transcript.blocks[index].id == id) return index;
    }
    for (usize index = 0; index < transcript.blocks.size(); ++index) {
        if (transcript.blocks[index].id == id) return index;
    }
    return std::nullopt;
}

usize find_anchor_row(const std::vector<LayoutRow> &rows, usize source_offset) {
    usize closest = 0;
    for (usize index = 0; index < rows.size(); ++index) {
        if (rows[index].source_offset > source_offset) break;
        closest = index;
    }
    return closest;
}

std::vector<LayoutRow> rows_from(const SessionScreen &screen, const ViewportAnchor &anchor, usize limit) {
    std::vector<LayoutRow> result;
    if (limit == 0) return result;
    const auto first_block = find_block(screen.transcript, anchor.block_id);
    if (!first_block) return result;

    for (usize block_index = *first_block; block_index < screen.transcript.blocks.size() && result.size() < limit; ++block_index) {
        auto rows = screen.layout_block(screen.transcript.blocks[block_index]);
        const auto first_row = block_index == *first_block ? find_anchor_row(rows, anchor.source_offset) : 0;
        const auto count = std::min(limit - result.size(), rows.size() - first_row);
        result.insert(result.end(), std::make_move_iterator(rows.begin() + static_cast<isize>(first_row)),
                      std::make_move_iterator(rows.begin() + static_cast<isize>(first_row + count)));
    }
    return result;
}

std::vector<LayoutRow> rows_before(const SessionScreen &screen, const ViewportAnchor &anchor, usize limit) {
    std::vector<LayoutRow> result;
    if (limit == 0) return result;
    const auto first_block = find_block(screen.transcript, anchor.block_id);
    if (!first_block) return result;

    auto block_index = *first_block;
    auto rows = screen.layout_block(screen.transcript.blocks[block_index]);
    auto end = find_anchor_row(rows, anchor.source_offset);
    while (true) {
        const auto count = std::min(limit - result.size(), end);
        result.insert(result.begin(), std::make_move_iterator(rows.begin() + static_cast<isize>(end - count)),
                      std::make_move_iterator(rows.begin() + static_cast<isize>(end)));
        if (result.size() == limit || block_index == 0) break;
        --block_index;
        rows = screen.layout_block(screen.transcript.blocks[block_index]);
        end = rows.size();
    }
    return result;
}

std::vector<LayoutRow> tail_rows(const SessionScreen &screen, usize limit) {
    std::vector<LayoutRow> result;
    if (limit == 0) return result;
    for (usize block_index = screen.transcript.blocks.size(); block_index > 0 && result.size() < limit; --block_index) {
        auto rows = screen.layout_block(screen.transcript.blocks[block_index - 1]);
        const auto count = std::min(limit - result.size(), rows.size());
        result.insert(result.begin(), std::make_move_iterator(rows.end() - static_cast<isize>(count)), std::make_move_iterator(rows.end()));
    }
    return result;
}

ViewportAnchor row_anchor(const LayoutRow &row) { return {.block_id = row.block_id, .source_offset = row.source_offset}; }

std::vector<LayoutRow> visible_rows(const SessionScreen &screen) {
    const auto visible = static_cast<usize>(std::max(screen.viewport_rows(), 0));
    if (visible == 0) return {};
    if (!screen.anchor) return tail_rows(screen, visible);

    auto rows = rows_from(screen, *screen.anchor, visible);
    if (rows.empty()) return tail_rows(screen, visible);
    if (rows.size() < visible) {
        auto previous = rows_before(screen, *screen.anchor, visible - rows.size());
        previous.insert(previous.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        return previous;
    }
    return rows;
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
        const auto grapheme = next_grapheme(composer.text, offset);
        const auto encoded = std::string_view(composer.text).substr(offset, grapheme.size);
        offset += grapheme.size;
        if (encoded == "\r") continue;
        if (encoded == "\n") {
            display.text += "\\n";
        } else if (encoded == "\t") {
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
        const auto grapheme = next_grapheme(text, offset);
        const auto encoded = text.substr(offset, grapheme.size);
        offset += grapheme.size;
        const auto cells = grapheme.width;
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
    const auto previous = previous_grapheme_boundary(text, cursor);
    text.erase(previous, cursor - previous);
    cursor = previous;
}

void Composer::erase() {
    const auto next = next_grapheme_boundary(text, cursor);
    text.erase(cursor, next - cursor);
}

void Composer::move_left() { cursor = previous_grapheme_boundary(text, cursor); }

void Composer::move_right() { cursor = next_grapheme_boundary(text, cursor); }

void Composer::move_home() noexcept { cursor = 0; }

void Composer::move_end() noexcept { cursor = text.size(); }

void Composer::clear() noexcept {
    text.clear();
    cursor = 0;
}

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
    if (std::holds_alternative<PromptSubmitted>(event)) state = SessionState::WAITING;
    if (std::holds_alternative<AssistantTextDelta>(event)) state = SessionState::STREAMING;
    if (std::holds_alternative<ToolStarted>(event)) state = SessionState::RUNNING_TOOLS;
    if (std::holds_alternative<ToolCompleted>(event)) state = SessionState::STREAMING;
    if (std::holds_alternative<TurnCompleted>(event)) state = SessionState::COMPLETED;
    if (std::holds_alternative<TurnCancelled>(event)) state = SessionState::CANCELLED;
    if (std::holds_alternative<TurnFailed>(event)) state = SessionState::FAILED;
    if (anchor) unread = true;
}

void SessionScreen::add_notice(std::string text) { apply(SessionNotice{.text = trim_notice(std::move(text))}); }

std::vector<LayoutRow> SessionScreen::layout_block(const Block &block) const {
    const bool stable = block.state != BlockState::STREAMING && block.state != BlockState::RUNNING;
    if (stable) {
        const auto cached = layout_cache.find(block.id);
        if (cached != layout_cache.end() && cached->second.columns == size.columns && cached->second.kind == block.kind &&
            cached->second.state == block.state && cached->second.text == block.text) {
            ++diagnostics.cache_hits;
            return cached->second.rows;
        }
    }

    ++diagnostics.cache_misses;
    ++diagnostics.blocks_laid_out;
    auto rows = wrap_block(block, size.columns);
    if (stable) {
        layout_cache.insert_or_assign(
            block.id,
            CachedBlockLayout{.columns = size.columns, .kind = block.kind, .state = block.state, .text = block.text, .rows = rows});
    }
    return rows;
}

LayoutDiagnostics SessionScreen::layout_diagnostics() const noexcept {
    auto result = diagnostics;
    result.cached_blocks = layout_cache.size();
    return result;
}

void SessionScreen::scroll(i32 row_delta) {
    if (row_delta == 0 || viewport_rows() <= 0) return;
    const auto current_rows = visible_rows(*this);
    if (current_rows.empty()) return;
    const auto current = row_anchor(current_rows.front());

    if (row_delta < 0) {
        const auto distance = static_cast<usize>(-static_cast<i64>(row_delta));
        auto previous = rows_before(*this, current, distance);
        if (!previous.empty()) anchor = row_anchor(previous.front());
        return;
    }

    const auto distance = static_cast<usize>(row_delta);
    auto following = rows_from(*this, current, distance + 1);
    if (following.size() <= distance) {
        follow_tail();
        return;
    }
    const auto target = row_anchor(following[distance]);
    const auto remaining = rows_from(*this, target, static_cast<usize>(viewport_rows()) + 1);
    if (remaining.size() <= static_cast<usize>(viewport_rows())) {
        follow_tail();
        return;
    }
    anchor = target;
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

    const auto rows = visible_rows(*this);
    for (usize index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
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
