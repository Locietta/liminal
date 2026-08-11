#include "headless.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <liminal/event.h>
#include <liminal/tui/surface.h>

namespace liminal::tui {

namespace {

constexpr i64 k_frame_interval_ms = 16;
constexpr i32 k_max_columns = 500;
constexpr i32 k_max_rows = 200;
constexpr usize k_max_actions = 10'000;
constexpr usize k_max_text_bytes = 1024 * 1024;
constexpr usize k_max_session_text_bytes = 8 * 1024 * 1024;
constexpr usize k_max_ansi_operations = 256;

std::string_view name(BlockKind kind) {
    switch (kind) {
        case BlockKind::USER: return "user";
        case BlockKind::ASSISTANT: return "assistant";
        case BlockKind::TOOL: return "tool";
        case BlockKind::NOTICE: return "notice";
    }
    return "notice";
}

std::string_view name(BlockState state) {
    switch (state) {
        case BlockState::STREAMING: return "streaming";
        case BlockState::RUNNING: return "running";
        case BlockState::COMPLETED: return "completed";
        case BlockState::CANCELLED: return "cancelled";
        case BlockState::FAILED: return "failed";
    }
    return "completed";
}

std::string_view name(Style style) {
    switch (style) {
        case Style::NORMAL: return "normal";
        case Style::EMPHASIS: return "emphasis";
        case Style::ITALIC: return "italic";
        case Style::MUTED: return "muted";
        case Style::ACCENT: return "accent";
        case Style::CODE: return "code";
        case Style::CODE_KEYWORD: return "code_keyword";
        case Style::CODE_PREPROCESSOR: return "code_preprocessor";
        case Style::CODE_TYPE: return "code_type";
        case Style::CODE_FUNCTION: return "code_function";
        case Style::CODE_STRING: return "code_string";
        case Style::CODE_COMMENT: return "code_comment";
        case Style::CODE_NUMBER: return "code_number";
        case Style::CODE_CONSTANT: return "code_constant";
        case Style::CODE_PROPERTY: return "code_property";
        case Style::CODE_OPERATOR: return "code_operator";
        case Style::LINK: return "link";
        case Style::DIFF_ADDITION: return "diff_addition";
        case Style::DIFF_DELETION: return "diff_deletion";
        case Style::DIFF_HUNK: return "diff_hunk";
        case Style::FAILURE: return "failure";
        case Style::COMPOSER: return "composer";
        case Style::FOOTER_MODEL: return "footer_model";
        case Style::FOOTER_WORKSPACE: return "footer_workspace";
        case Style::FOOTER_CONTEXT: return "footer_context";
        case Style::FOOTER_TOKENS: return "footer_tokens";
    }
    return "normal";
}

std::string_view name(SessionState state) {
    switch (state) {
        case SessionState::EDITING: return "editing";
        case SessionState::WAITING: return "waiting";
        case SessionState::STREAMING: return "streaming";
        case SessionState::RUNNING_TOOLS: return "running_tools";
        case SessionState::COMPLETED: return "completed";
        case SessionState::CANCELLED: return "cancelled";
        case SessionState::FAILED: return "failed";
    }
    return "editing";
}

std::expected<void, std::string> validate_size(i32 columns, i32 rows) {
    if (columns < 1 || columns > k_max_columns || rows < 1 || rows > k_max_rows) {
        return std::unexpected("terminal size must be within 1..500 columns and 1..200 rows");
    }
    return {};
}

std::string visible_ansi(std::string_view operation) {
    std::string output;
    output.reserve(operation.size());
    for (const auto byte : operation) {
        switch (static_cast<unsigned char>(byte)) {
            case 0x1b: output += "\\x1b"; break;
            case '\r': output += "\\r"; break;
            case '\n': output += "\\n"; break;
            default: output += byte;
        }
    }
    return output;
}

} // namespace

HeadlessSession::HeadlessSession(i32 columns, i32 rows, i64 initial_now_ms)
    : screen([this] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(now_ms)); }),
      now_ms(std::max(initial_now_ms, i64{0})) {
    screen.resize({.columns = std::clamp(columns, 1, k_max_columns), .rows = std::clamp(rows, 1, k_max_rows)});
    screen.set_model("headless", std::nullopt);
    invalidate();
    flush();
}

std::expected<void, std::string> HeadlessSession::apply(const HeadlessAction &action) {
    if (action_count >= k_max_actions) return std::unexpected("session action limit exceeded");
    if (action.text.size() > k_max_text_bytes) return std::unexpected("action text limit exceeded");
    const auto action_bytes = action.text.size() + action.call_id.size() + action.name.size() + action.command.size() +
                              (action.effort ? action.effort->size() : usize{0});
    if (text_bytes + action_bytes > k_max_session_text_bytes) return std::unexpected("session text limit exceeded");
    text_bytes += action_bytes;
    ++action_count;

    if (action.type == "insert") {
        screen.insert(action.text);
    } else if (action.type == "backspace") {
        screen.backspace();
    } else if (action.type == "delete") {
        screen.erase();
    } else if (action.type == "backspace_word") {
        screen.backspace_word();
    } else if (action.type == "delete_word") {
        screen.erase_word();
    } else if (action.type == "left") {
        screen.move_left();
    } else if (action.type == "right") {
        screen.move_right();
    } else if (action.type == "word_left") {
        screen.move_word_left();
    } else if (action.type == "word_right") {
        screen.move_word_right();
    } else if (action.type == "up") {
        screen.move_up();
    } else if (action.type == "down") {
        screen.move_down();
    } else if (action.type == "home") {
        screen.move_home();
    } else if (action.type == "end") {
        screen.move_end();
    } else if (action.type == "document_home") {
        screen.move_document_home();
    } else if (action.type == "document_end") {
        screen.move_document_end();
    } else if (action.type == "clear") {
        screen.clear_prompt();
    } else if (action.type == "submit") {
        auto prompt = screen.take_prompt();
        if (!action.text.empty()) {
            prompt = action.text;
            screen.prompt_history.record(prompt);
        }
        screen.apply(PromptSubmitted{.text = std::move(prompt)});
    } else if (action.type == "assistant_delta") {
        screen.apply(AssistantTextDelta{.text = action.text});
    } else if (action.type == "assistant_segment_completed") {
        screen.apply(AssistantSegmentCompleted{});
    } else if (action.type == "tool_started") {
        screen.apply(ToolStarted{.call_id = action.call_id, .name = action.name, .description = action.text, .command = action.command});
    } else if (action.type == "tool_completed") {
        screen.apply(ToolCompleted{
            .call_id = action.call_id,
            .name = action.name,
            .command = action.command,
            .summary = action.text,
            .is_error = action.is_error,
        });
    } else if (action.type == "turn_completed") {
        screen.apply(TurnCompleted{});
    } else if (action.type == "turn_cancelled") {
        screen.apply(TurnCancelled{});
    } else if (action.type == "turn_failed") {
        screen.apply(TurnFailed{.message = action.text});
    } else if (action.type == "notice") {
        screen.add_notice(action.text);
    } else if (action.type == "scroll") {
        screen.scroll(action.amount);
    } else if (action.type == "page_up") {
        screen.page(-1);
    } else if (action.type == "page_down") {
        screen.page(1);
    } else if (action.type == "follow_tail") {
        screen.follow_tail();
    } else if (action.type == "resize") {
        if (auto valid = validate_size(action.columns, action.rows); !valid) return valid;
        screen.resize({.columns = action.columns, .rows = action.rows});
    } else if (action.type == "set_model") {
        screen.apply(ModelSelected{.name = action.name, .effort = action.effort});
    } else if (action.type == "advance_time") {
        if (action.milliseconds < 0) return std::unexpected("virtual time cannot move backwards");
        now_ms += action.milliseconds;
        if (screen.animating()) invalidate();
        if (render_pending && now_ms >= render_at_ms) flush();
        return {};
    } else if (action.type == "flush") {
        flush();
        return {};
    } else {
        return std::unexpected("unknown headless action: " + action.type);
    }

    invalidate();
    return {};
}

std::expected<void, std::string> HeadlessSession::apply(const std::vector<HeadlessAction> &actions) {
    for (const auto &action : actions) {
        if (auto result = apply(action); !result) return result;
    }
    return {};
}

HeadlessSnapshot HeadlessSession::inspect() const {
    // Projection populates mutable layout caches. Inspect a copy so this
    // read-only operation cannot alter future diagnostics or rendering.
    auto projected_screen = screen;
    const auto frame = projected_screen.frame();
    HeadlessSnapshot snapshot{.now_ms = now_ms,
                              .render_pending = render_pending,
                              .render_count = render_count,
                              .action_count = action_count,
                              .text_bytes = text_bytes,
                              .columns = screen.size.columns,
                              .rows = screen.size.rows,
                              .model = screen.model,
                              .semantic_state = std::string(name(screen.state)),
                              .effort = screen.effort,
                              .composer_text = screen.composer.text,
                              .composer_cursor = screen.composer.cursor,
                              .unread = screen.unread,
                              .cursor = {.row = frame.cursor.row, .column = frame.cursor.column, .visible = frame.cursor.visible},
                              .layout = projected_screen.layout_diagnostics(),
                              .ansi_operations = {}};
    for (const auto &operation : ansi_operations) snapshot.ansi_operations.push_back(visible_ansi(operation));
    if (screen.anchor) snapshot.anchor = {.block_id = screen.anchor->block_id, .source_offset = screen.anchor->source_offset};
    for (const auto &block : screen.transcript.blocks) {
        snapshot.blocks.push_back({.id = block.id,
                                   .kind = std::string(name(block.kind)),
                                   .state = std::string(name(block.state)),
                                   .text = block.text,
                                   .detail = block.detail,
                                   .command = block.command,
                                   .call_id = block.call_id});
    }
    for (i32 row = 0; row < frame.surface.rows; ++row) snapshot.visible_text.push_back(frame.surface.row_text(row));
    for (i32 row = 0; row < frame.surface.rows; ++row) {
        for (i32 column = 0; column < frame.surface.columns; ++column) {
            const auto &cell = frame.surface.cells[static_cast<usize>(row * frame.surface.columns + column)];
            if (cell.text == " " && cell.style == Style::NORMAL && !cell.continuation) continue;
            snapshot.cells.push_back({.row = row,
                                      .column = column,
                                      .text = cell.text,
                                      .style = std::string(name(cell.style)),
                                      .continuation = cell.continuation});
        }
    }
    return snapshot;
}

void HeadlessSession::invalidate() {
    if (render_pending) return;
    render_pending = true;
    render_at_ms = now_ms + k_frame_interval_ms;
}

void HeadlessSession::flush() {
    auto frame = screen.frame();
    auto encoded = encode_frame_diff(previous_frame ? &*previous_frame : nullptr, frame);
    if (!encoded.empty()) {
        if (ansi_operations.size() == k_max_ansi_operations) ansi_operations.erase(ansi_operations.begin());
        ansi_operations.push_back(std::move(encoded));
        ++render_count;
    }
    previous_frame = std::move(frame);
    render_pending = false;
}

} // namespace liminal::tui
