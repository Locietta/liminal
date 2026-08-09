#include "session_screen.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <liminal/tui/rich_text.h>

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
    if (block.kind == BlockKind::ASSISTANT) {
        auto rich = layout_rich_text(block.text, columns);
        std::vector<LayoutRow> rows;
        rows.reserve(rich.size());
        for (auto &row : rich) {
            rows.push_back({.block_id = block.id, .source_offset = row.source_offset, .spans = std::move(row.spans)});
        }
        return rows;
    }

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

usize line_start(std::string_view text, usize cursor) {
    if (cursor == 0) return 0;
    const auto newline = text.rfind('\n', cursor - 1);
    return newline == std::string_view::npos ? 0 : newline + 1;
}

usize line_end(std::string_view text, usize cursor) {
    const auto newline = text.find('\n', cursor);
    return newline == std::string_view::npos ? text.size() : newline;
}

i32 composer_width(std::string_view text) {
    i32 width = 0;
    usize offset = 0;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        const auto encoded = text.substr(offset, grapheme.size);
        offset += grapheme.size;
        if (encoded == "\r" || encoded == "\n") continue;
        width += encoded == "\t" ? 2 : std::max(grapheme.width, 0);
    }
    return width;
}

usize offset_at_column(std::string_view text, usize first, usize last, i32 column) {
    i32 used = 0;
    auto offset = first;
    while (offset < last) {
        const auto grapheme = next_grapheme(text, offset);
        const auto encoded = text.substr(offset, grapheme.size);
        const auto width = encoded == "\t" ? 2 : std::max(grapheme.width, 0);
        if (width > 0 && used + width > column) break;
        used += width;
        offset += grapheme.size;
    }
    return offset;
}

bool space_grapheme(std::string_view value) {
    return value == "\n" || value == "\r" || value == "\t" || value == " " ||
           (value.size() == 1 && std::isspace(static_cast<unsigned char>(value.front())) != 0);
}

bool word_grapheme(std::string_view value) {
    if (value.size() != 1) return !space_grapheme(value);
    const auto character = static_cast<unsigned char>(value.front());
    return std::isalnum(character) != 0 || character == '_';
}

usize previous_word_boundary(std::string_view text, usize cursor) {
    auto offset = cursor;
    while (offset > 0) {
        const auto previous = previous_grapheme_boundary(text, offset);
        if (!space_grapheme(text.substr(previous, offset - previous))) break;
        offset = previous;
    }
    if (offset == 0) return 0;
    auto previous = previous_grapheme_boundary(text, offset);
    const bool word = word_grapheme(text.substr(previous, offset - previous));
    while (offset > 0) {
        previous = previous_grapheme_boundary(text, offset);
        const auto value = text.substr(previous, offset - previous);
        if (space_grapheme(value) || word_grapheme(value) != word) break;
        offset = previous;
    }
    return offset;
}

usize next_word_boundary(std::string_view text, usize cursor) {
    auto offset = cursor;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        const auto value = text.substr(offset, grapheme.size);
        if (!space_grapheme(value)) break;
        offset += grapheme.size;
    }
    if (offset == text.size()) return offset;
    auto grapheme = next_grapheme(text, offset);
    const bool word = word_grapheme(text.substr(offset, grapheme.size));
    while (offset < text.size()) {
        grapheme = next_grapheme(text, offset);
        const auto value = text.substr(offset, grapheme.size);
        if (space_grapheme(value) || word_grapheme(value) != word) break;
        offset += grapheme.size;
    }
    return offset;
}

struct ComposerProjection {
    std::vector<std::string> rows;
    i32 cursor_row = 0;
    i32 cursor_column = 0;
};

std::string composer_prefix(const SessionScreen &screen) {
    auto prefix = screen.model;
    if (screen.effort) prefix += "@" + *screen.effort;
    prefix += " > ";
    if (text_width(prefix) < screen.size.columns) return prefix;
    return screen.size.columns >= 3 ? "> " : std::string{};
}

ComposerProjection project_composer(const SessionScreen &screen) {
    ComposerProjection result;
    const auto columns = std::max(screen.size.columns, 1);
    std::string current = composer_prefix(screen);
    i32 used = text_width(current);
    bool cursor_set = false;
    usize offset = 0;
    while (offset < screen.composer.text.size()) {
        const auto start = offset;
        const auto grapheme = next_grapheme(screen.composer.text, offset);
        const auto encoded = std::string_view(screen.composer.text).substr(offset, grapheme.size);
        offset += grapheme.size;
        if (encoded == "\r") continue;
        if (encoded == "\n") {
            if (screen.composer.cursor == start) {
                result.cursor_row = static_cast<i32>(result.rows.size());
                result.cursor_column = used;
                cursor_set = true;
            }
            result.rows.push_back(std::move(current));
            current.clear();
            used = 0;
            continue;
        }

        const auto displayed = encoded == "\t" ? std::string_view("\\t") : encoded;
        const auto width = encoded == "\t" ? 2 : std::max(grapheme.width, 0);
        if (width > 0 && used > 0 && used + width > columns) {
            result.rows.push_back(std::move(current));
            current.clear();
            used = 0;
        }
        if (screen.composer.cursor == start) {
            result.cursor_row = static_cast<i32>(result.rows.size());
            result.cursor_column = used;
            cursor_set = true;
        }
        current += displayed;
        used += width;
    }
    if (!cursor_set || screen.composer.cursor == screen.composer.text.size()) {
        result.cursor_row = static_cast<i32>(result.rows.size());
        result.cursor_column = used;
    }
    result.rows.push_back(std::move(current));
    return result;
}

i32 composer_height(const SessionScreen &screen, const ComposerProjection &projection) {
    const auto header = screen.size.rows >= 2 ? 1 : 0;
    const auto status = screen.size.rows >= 3 ? 1 : 0;
    const auto available = std::max(screen.size.rows - header - status, 1);
    const auto growth_limit = std::max(std::min(screen.size.rows / 3, 8), 1);
    return std::min({static_cast<i32>(projection.rows.size()), growth_limit, available});
}

std::string trim_notice(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

} // namespace

void Composer::insert(std::string_view value) {
    text.insert(cursor, value);
    cursor += value.size();
    preferred_column.reset();
}

void Composer::backspace() {
    const auto previous = previous_grapheme_boundary(text, cursor);
    text.erase(previous, cursor - previous);
    cursor = previous;
    preferred_column.reset();
}

void Composer::erase() {
    const auto next = next_grapheme_boundary(text, cursor);
    text.erase(cursor, next - cursor);
    preferred_column.reset();
}

void Composer::backspace_word() {
    const auto previous = previous_word_boundary(text, cursor);
    text.erase(previous, cursor - previous);
    cursor = previous;
    preferred_column.reset();
}

void Composer::erase_word() {
    const auto next = next_word_boundary(text, cursor);
    text.erase(cursor, next - cursor);
    preferred_column.reset();
}

void Composer::move_left() {
    cursor = previous_grapheme_boundary(text, cursor);
    preferred_column.reset();
}

void Composer::move_right() {
    cursor = next_grapheme_boundary(text, cursor);
    preferred_column.reset();
}

void Composer::move_word_left() {
    cursor = previous_word_boundary(text, cursor);
    preferred_column.reset();
}

void Composer::move_word_right() {
    cursor = next_word_boundary(text, cursor);
    preferred_column.reset();
}

bool Composer::move_up() {
    const auto current_start = line_start(text, cursor);
    if (current_start == 0) return false;
    const auto target_column =
        preferred_column.value_or(composer_width(std::string_view(text).substr(current_start, cursor - current_start)));
    const auto previous_end = current_start - 1;
    const auto previous_start = line_start(text, previous_end);
    cursor = offset_at_column(text, previous_start, previous_end, target_column);
    preferred_column = target_column;
    return true;
}

bool Composer::move_down() {
    const auto current_start = line_start(text, cursor);
    const auto current_end = line_end(text, cursor);
    if (current_end == text.size()) return false;
    const auto target_column =
        preferred_column.value_or(composer_width(std::string_view(text).substr(current_start, cursor - current_start)));
    const auto next_start = current_end + 1;
    const auto next_end = line_end(text, next_start);
    cursor = offset_at_column(text, next_start, next_end, target_column);
    preferred_column = target_column;
    return true;
}

void Composer::move_home() {
    cursor = line_start(text, cursor);
    preferred_column.reset();
}

void Composer::move_end() {
    cursor = line_end(text, cursor);
    preferred_column.reset();
}

void Composer::move_document_home() noexcept {
    cursor = 0;
    preferred_column.reset();
}

void Composer::move_document_end() noexcept {
    cursor = text.size();
    preferred_column.reset();
}

void Composer::replace(std::string value) {
    text = std::move(value);
    cursor = text.size();
    preferred_column.reset();
}

void Composer::clear() noexcept {
    text.clear();
    cursor = 0;
    preferred_column.reset();
}

std::string Composer::take() {
    cursor = 0;
    preferred_column.reset();
    return std::exchange(text, {});
}

void PromptHistory::record(const std::string &prompt) {
    index.reset();
    draft.clear();
    if (prompt.empty() || (!entries.empty() && entries.back() == prompt)) return;
    constexpr usize k_max_entries = 100;
    if (entries.size() == k_max_entries) entries.erase(entries.begin());
    entries.push_back(prompt);
}

bool PromptHistory::previous(Composer &composer) {
    if (entries.empty()) return false;
    if (!index) {
        draft = composer.text;
        index = entries.size();
    }
    if (*index == 0) return false;
    --*index;
    composer.replace(entries[*index]);
    return true;
}

bool PromptHistory::next(Composer &composer) {
    if (!index) return false;
    ++*index;
    if (*index < entries.size()) {
        composer.replace(entries[*index]);
        return true;
    }
    composer.replace(std::exchange(draft, {}));
    index.reset();
    return true;
}

void PromptHistory::edited() noexcept {
    index.reset();
    draft.clear();
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
    if (const auto *notice = std::get_if<SessionNotice>(&event)) {
        transcript.apply(SessionNotice{.text = trim_notice(notice->text)});
    } else {
        transcript.apply(event);
    }
    if (const auto *selected = std::get_if<ModelSelected>(&event)) set_model(selected->name, selected->effort);
    if (std::holds_alternative<PromptSubmitted>(event)) state = SessionState::WAITING;
    if (std::holds_alternative<AssistantTextDelta>(event)) state = SessionState::STREAMING;
    if (std::holds_alternative<ToolStarted>(event)) state = SessionState::RUNNING_TOOLS;
    if (std::holds_alternative<ToolCompleted>(event)) {
        const bool running = std::ranges::any_of(
            transcript.blocks, [](const Block &block) { return block.kind == BlockKind::TOOL && block.state == BlockState::RUNNING; });
        state = running ? SessionState::RUNNING_TOOLS : SessionState::STREAMING;
    }
    if (std::holds_alternative<TurnCompleted>(event)) state = SessionState::COMPLETED;
    if (std::holds_alternative<TurnCancelled>(event)) state = SessionState::CANCELLED;
    if (std::holds_alternative<TurnFailed>(event)) state = SessionState::FAILED;
    if (anchor) unread = true;
}

void SessionScreen::add_notice(std::string text) { apply(SessionNotice{.text = trim_notice(std::move(text))}); }

void SessionScreen::insert(std::string_view text) {
    prompt_history.edited();
    composer.insert(text);
    mark_editing();
}

void SessionScreen::backspace() {
    prompt_history.edited();
    composer.backspace();
    mark_editing();
}

void SessionScreen::erase() {
    prompt_history.edited();
    composer.erase();
    mark_editing();
}

void SessionScreen::backspace_word() {
    prompt_history.edited();
    composer.backspace_word();
    mark_editing();
}

void SessionScreen::erase_word() {
    prompt_history.edited();
    composer.erase_word();
    mark_editing();
}

void SessionScreen::move_left() {
    composer.move_left();
    mark_editing();
}

void SessionScreen::move_right() {
    composer.move_right();
    mark_editing();
}

void SessionScreen::move_word_left() {
    composer.move_word_left();
    mark_editing();
}

void SessionScreen::move_word_right() {
    composer.move_word_right();
    mark_editing();
}

void SessionScreen::move_up() {
    if (composer.move_up()) {
        mark_editing();
        return;
    }
    scroll(-1);
}

void SessionScreen::move_down() {
    if (composer.move_down()) {
        mark_editing();
        return;
    }
    scroll(1);
}

void SessionScreen::previous_prompt() {
    prompt_history.previous(composer);
    mark_editing();
}

void SessionScreen::next_prompt() {
    prompt_history.next(composer);
    mark_editing();
}

void SessionScreen::move_home() {
    composer.move_home();
    mark_editing();
}

void SessionScreen::move_end() {
    composer.move_end();
    mark_editing();
}

void SessionScreen::move_document_home() {
    composer.move_document_home();
    mark_editing();
}

void SessionScreen::move_document_end() {
    composer.move_document_end();
    mark_editing();
}

void SessionScreen::replace_prompt(std::string text) {
    prompt_history.edited();
    composer.replace(std::move(text));
    mark_editing();
}

void SessionScreen::clear_prompt() {
    prompt_history.edited();
    composer.clear();
    mark_editing();
}

std::string SessionScreen::take_prompt() {
    auto prompt = composer.take();
    prompt_history.record(prompt);
    return prompt;
}

void SessionScreen::mark_editing() noexcept {
    if (state != SessionState::WAITING && state != SessionState::STREAMING && state != SessionState::RUNNING_TOOLS) {
        state = SessionState::EDITING;
    }
}

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
    const auto tail = tail_rows(*this, static_cast<usize>(viewport_rows()));
    if (!tail.empty() && target == row_anchor(tail.front())) {
        follow_tail();
        return;
    }
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

i32 SessionScreen::viewport_rows() const {
    const auto projection = project_composer(*this);
    const auto header = size.rows >= 2 ? 1 : 0;
    const auto status = size.rows >= 3 ? 1 : 0;
    return std::max(size.rows - header - status - composer_height(*this, projection), 0);
}

Frame SessionScreen::frame() const {
    Frame result{.surface = Surface(size.columns, size.rows)};
    if (size.rows <= 0 || size.columns <= 0) return result;

    const bool header = size.rows >= 2;
    const bool status = size.rows >= 3;
    const auto projected_composer = project_composer(*this);
    const auto prompt_rows = composer_height(*this, projected_composer);
    const auto prompt_row = size.rows - prompt_rows;
    if (header) {
        auto selection = model;
        if (effort) selection += "@" + *effort;
        result.surface.write(0, 0, "liminal  " + selection, Style::EMPHASIS);
    }

    const auto rows = visible_rows(*this);
    for (usize index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
        const auto target_row = static_cast<i32>(index) + (header ? 1 : 0);
        if (row.spans.empty()) {
            result.surface.write(target_row, 0, row.text, row.style);
            continue;
        }
        i32 column = 0;
        for (const auto &span : row.spans) column = result.surface.write(target_row, column, span.text, span.style);
    }

    if (status) {
        std::string status_text;
        if (external_editor_active) {
            status_text = "Save and close external editor to continue";
        } else if (anchor) {
            status_text = "history";
            if (unread) status_text += " | new output";
        } else {
            status_text = "Up/Down/wheel scroll | Ctrl+Up/Down prompts | Ctrl+G editor | Ctrl+J newline | Enter send";
        }
        result.surface.write(prompt_row - 1, 0, status_text, Style::MUTED);
    }

    const auto first_composer_row = std::max(projected_composer.cursor_row - prompt_rows + 1, 0);
    for (i32 index = 0; index < prompt_rows; ++index) {
        const auto source = first_composer_row + index;
        if (source >= static_cast<i32>(projected_composer.rows.size())) break;
        result.surface.write(prompt_row + index, 0, projected_composer.rows[static_cast<usize>(source)], Style::NORMAL);
    }
    result.cursor = {
        .row = prompt_row + projected_composer.cursor_row - first_composer_row,
        .column = std::clamp(projected_composer.cursor_column, 0, size.columns - 1),
        .visible = true,
    };
    return result;
}

} // namespace liminal::tui
