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
        case Style::ASSISTANT: return "assistant";
        case Style::EMPHASIS: return "emphasis";
        case Style::ITALIC: return "italic";
        case Style::QUOTE: return "quote";
        case Style::QUOTE_EMPHASIS: return "quote_emphasis";
        case Style::QUOTE_ITALIC: return "quote_italic";
        case Style::MUTED: return "muted";
        case Style::ACCENT: return "accent";
        case Style::CODE: return "code";
        case Style::CODE_BLOCK: return "code_block";
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
        case Style::WORKING_BASE: return "working_base";
        case Style::WORKING_LOW: return "working_low";
        case Style::WORKING_MEDIUM: return "working_medium";
        case Style::WORKING_HIGH: return "working_high";
        case Style::WORKING_BRIGHT: return "working_bright";
        case Style::WORKING_PEAK: return "working_peak";
        case Style::FOOTER_MODEL: return "footer_model";
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

std::string_view name(SelectableListEffect effect) {
    switch (effect) {
        case SelectableListEffect::NONE: return "none";
        case SelectableListEffect::LOAD_PREVIOUS_PAGE: return "load_previous_page";
        case SelectableListEffect::LOAD_NEXT_PAGE: return "load_next_page";
        case SelectableListEffect::REPLACE_RESULTS: return "replace_results";
        case SelectableListEffect::CONFIRMED: return "confirmed";
        case SelectableListEffect::CANCELLED: return "cancelled";
    }
    return "none";
}

std::vector<CompactPickerItem> compact_picker_items(std::string_view text) {
    std::vector<CompactPickerItem> items;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto row = text.substr(0, end);
        if (!row.empty()) {
            CompactPickerItem item;
            item.current = row.starts_with('*');
            if (item.current) row.remove_prefix(1);
            item.id = std::string(row);
            item.primary = std::string(row);
            item.haystacks.push_back(item.id);
            items.push_back(std::move(item));
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return items;
}

SelectableListPage selectable_page(std::string_view text, bool has_more) {
    SelectableListPage page{.has_more = has_more};
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto row = text.substr(0, end);
        if (!row.empty()) page.items.push_back({.id = std::string(row), .primary = std::string(row)});
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return page;
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
                              action.preview.size() + (action.effort ? action.effort->size() : usize{0}) +
                              (action.home_directory ? action.home_directory->size() : usize{0}) +
                              (action.title ? action.title->size() : usize{0});
    if (text_bytes + action_bytes > k_max_session_text_bytes) return std::unexpected("session text limit exceeded");
    text_bytes += action_bytes;
    ++action_count;

    if (action.type == "insert") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::INSERT, action.text);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::INSERT, action.text);
        else
            screen.insert(action.text);
    } else if (action.type == "backspace") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::BACKSPACE);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::BACKSPACE);
        else
            screen.backspace();
    } else if (action.type == "delete") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::ERASE);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::ERASE);
        else
            screen.erase();
    } else if (action.type == "backspace_word") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::BACKSPACE_WORD);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::BACKSPACE_WORD);
        else
            screen.backspace_word();
    } else if (action.type == "delete_word") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::ERASE_WORD);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::ERASE_WORD);
        else
            screen.erase_word();
    } else if (action.type == "left") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::LEFT);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::LEFT);
        else
            screen.move_left();
    } else if (action.type == "right") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::RIGHT);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::RIGHT);
        else
            screen.move_right();
    } else if (action.type == "word_left") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::WORD_LEFT);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::WORD_LEFT);
        else
            screen.move_word_left();
    } else if (action.type == "word_right") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::WORD_RIGHT);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::WORD_RIGHT);
        else
            screen.move_word_right();
    } else if (action.type == "up") {
        if (selectable_list)
            selection_effect = selectable_list->apply(SelectableListAction::UP);
        else if (screen.apply_picker_key(PickerKey::UP) == PickerKeyResult::PASS)
            screen.move_up();
    } else if (action.type == "down") {
        if (selectable_list)
            selection_effect = selectable_list->apply(SelectableListAction::DOWN);
        else if (screen.apply_picker_key(PickerKey::DOWN) == PickerKeyResult::PASS)
            screen.move_down();
    } else if (action.type == "tab") {
        if (!selectable_list && screen.apply_picker_key(PickerKey::TAB) == PickerKeyResult::PASS) screen.insert("\t");
    } else if (action.type == "home") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::HOME);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::HOME);
        else
            screen.move_home();
    } else if (action.type == "end") {
        if (selectable_list)
            selection_effect = selectable_list->edit_query(PickerQueryEdit::END);
        else if (screen.model_picker_active())
            screen.picker_query_edit(PickerQueryEdit::END);
        else
            screen.move_end();
    } else if (action.type == "document_home") {
        screen.move_document_home();
    } else if (action.type == "document_end") {
        screen.move_document_end();
    } else if (action.type == "clear") {
        screen.clear_prompt();
    } else if (action.type == "submit") {
        if (selectable_list) {
            selection_effect = selectable_list->apply(SelectableListAction::CONFIRM);
        } else if (screen.apply_picker_key(PickerKey::ENTER) != PickerKeyResult::HANDLED) {
            auto prompt = screen.take_prompt();
            if (!action.text.empty()) {
                prompt = action.text;
                screen.prompt_history.record(prompt);
            }
            begin_activity_task();
            screen.apply(PromptSubmitted{.text = std::move(prompt)});
        }
    } else if (action.type == "escape") {
        if (selectable_list)
            selection_effect = selectable_list->apply(SelectableListAction::CANCEL);
        else if (screen.model_picker_active())
            screen.close_picker();
        else
            screen.apply_picker_key(PickerKey::ESCAPE);
    } else if (action.type == "assistant_delta") {
        screen.apply(AssistantTextDelta{.item_id = action.item_id, .text = action.text, .activity_scope = current_activity_scope()});
    } else if (action.type == "assistant_message_completed") {
        screen.apply(AssistantMessageCompleted{.item_id = action.item_id, .text = action.text, .activity_scope = current_activity_scope()});
    } else if (action.type == "tool_started") {
        screen.apply(ToolStarted{.call_id = action.call_id,
                                 .name = action.name,
                                 .description = action.text,
                                 .command = action.command,
                                 .activity_scope = current_activity_scope()});
    } else if (action.type == "tool_completed") {
        screen.apply(ToolCompleted{
            .call_id = action.call_id,
            .name = action.name,
            .command = action.command,
            .summary = action.text,
            .is_error = action.is_error,
            .activity_scope = current_activity_scope(),
        });
    } else if (action.type == "provider_activity_completed") {
        screen.apply(ProviderActivityCompleted{.activity_scope = current_activity_scope()});
        activity_scope.reset();
    } else if (action.type == "task_completed") {
        if (!activity_task_generation) begin_activity_task();
        screen.apply(TaskCompleted{.task_generation = *activity_task_generation});
        finish_activity_task();
    } else if (action.type == "task_cancelled") {
        screen.apply(TaskCancelled{});
        finish_activity_task();
    } else if (action.type == "task_failed") {
        screen.apply(TaskFailed{.message = action.text});
        finish_activity_task();
    } else if (action.type == "notice") {
        screen.add_notice(action.text);
    } else if (action.type == "scroll") {
        screen.scroll(action.amount);
    } else if (action.type == "page_up") {
        if (selectable_list)
            selection_effect = selectable_list->apply(SelectableListAction::PAGE_UP);
        else
            screen.page(-1);
    } else if (action.type == "page_down") {
        if (selectable_list)
            selection_effect = selectable_list->apply(SelectableListAction::PAGE_DOWN);
        else
            screen.page(1);
    } else if (action.type == "follow_tail") {
        screen.follow_tail();
    } else if (action.type == "resize") {
        if (auto valid = validate_size(action.columns, action.rows); !valid) return valid;
        screen.resize({.columns = action.columns, .rows = action.rows});
    } else if (action.type == "set_model") {
        screen.apply(ModelSelected{.name = action.name, .effort = action.effort});
    } else if (action.type == "set_header") {
        screen.set_header({.workspace_path = action.text,
                           .home_directory = action.home_directory,
                           .explicit_title = action.title,
                           .prompt_preview = action.preview});
    } else if (action.type == "set_session_title") {
        screen.set_session_title(action.title, action.preview);
    } else if (action.type == "open_list") {
        selectable_list.emplace(action.name.empty() ? "Select" : action.name, action.command.empty() ? "No items" : action.command,
                                selectable_page(action.text, action.amount != 0));
        if (action.is_error) selectable_list->enable_query("No matches");
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "open_loading_list") {
        selectable_list.emplace(action.name.empty() ? "Select" : action.name, action.command.empty() ? "No items" : action.command,
                                SelectableListPage{});
        selectable_list->enable_query("No matches");
        selectable_list->begin_query_load();
        selection_effect = SelectableListEffect::REPLACE_RESULTS;
    } else if (action.type == "list_next_page") {
        if (!selectable_list || !selectable_list->waiting_for_page) return std::unexpected("selectable list is not loading a page");
        selectable_list->append_page(selectable_page(action.text, action.amount != 0));
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "list_previous_page") {
        if (!selectable_list || !selectable_list->waiting_for_page) return std::unexpected("selectable list is not loading a page");
        selectable_list->prepend_page(selectable_page(action.text, action.amount != 0));
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "list_replace") {
        if (!selectable_list || !selectable_list->waiting_for_query) return std::unexpected("selectable list is not replacing results");
        selectable_list->replace_page(selectable_page(action.text, action.amount != 0));
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "list_page_error") {
        if (!selectable_list || !selectable_list->waiting_for_page) return std::unexpected("selectable list is not loading a page");
        selectable_list->fail_page(action.text);
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "list_query_error") {
        if (!selectable_list || !selectable_list->waiting_for_query) return std::unexpected("selectable list is not replacing results");
        selectable_list->fail_query(action.text);
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "close_list") {
        selectable_list.reset();
        selection_effect = SelectableListEffect::NONE;
    } else if (action.type == "open_model_picker") {
        CompactPicker picker{.query_label = action.name.empty() ? "Model" : action.name,
                             .empty_message = action.command.empty() ? "No matching model" : action.command};
        picker.loading = action.amount != 0;
        picker.set_items(compact_picker_items(action.text));
        screen.open_picker(std::move(picker));
    } else if (action.type == "picker_items") {
        if (!screen.model_picker_active()) return std::unexpected("no compact picker is open");
        screen.picker_set_items(compact_picker_items(action.text));
    } else if (action.type == "picker_error") {
        if (!screen.model_picker_active()) return std::unexpected("no compact picker is open");
        screen.picker_fail(action.text);
    } else if (action.type == "close_picker") {
        screen.close_picker();
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

void HeadlessSession::begin_activity_task() noexcept {
    activity_task_generation = next_activity_task_generation++;
    activity_scope.reset();
}

ActivityScope HeadlessSession::current_activity_scope() noexcept {
    if (!activity_task_generation) begin_activity_task();
    if (!activity_scope) {
        activity_scope = ActivityScope{
            .task_generation = *activity_task_generation,
            .provider_call_generation = next_activity_provider_call_generation++,
        };
    }
    return *activity_scope;
}

void HeadlessSession::finish_activity_task() noexcept {
    activity_scope.reset();
    activity_task_generation.reset();
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
    const auto frame = selectable_list ? selectable_list->frame(screen.size.columns, screen.size.rows) : projected_screen.frame();
    HeadlessSnapshot snapshot{.now_ms = now_ms,
                              .render_pending = render_pending,
                              .render_count = render_count,
                              .action_count = action_count,
                              .text_bytes = text_bytes,
                              .columns = screen.size.columns,
                              .rows = screen.size.rows,
                              .model = screen.model,
                              .workspace_path = screen.header.workspace_path,
                              .session_title = resolve_session_title(screen.header),
                              .semantic_state = std::string(name(screen.state)),
                              .focused_surface = selectable_list              ? "selectable_list" :
                                                 screen.model_picker_active() ? "compact_picker" :
                                                                                "session",
                              .selection_effect = std::string(name(selection_effect)),
                              .selected_id = selectable_list && selectable_list->selected_id() ?
                                                 std::optional<std::string>(*selectable_list->selected_id()) :
                                                 std::nullopt,
                              .effort = screen.effort,
                              .composer_text = screen.composer.text,
                              .composer_cursor = screen.composer.cursor,
                              .cursor = {.row = frame.cursor.row, .column = frame.cursor.column, .visible = frame.cursor.visible},
                              .layout = projected_screen.layout_diagnostics(),
                              .ansi_operations = {}};
    for (const auto &operation : ansi_operations) snapshot.ansi_operations.push_back(visible_ansi(operation));
    if (screen.anchor) snapshot.anchor = {.block_id = screen.anchor->block_id, .source_offset = screen.anchor->source_offset};
    snapshot.command_menu_open = screen.command_menu.open;
    if (const auto *active = screen.picker ? &*screen.picker : (screen.command_menu.open ? &screen.command_menu.picker : nullptr)) {
        snapshot.picker_query = active->query;
        snapshot.picker_query_cursor = active->query_cursor;
        if (active->highlighted_id()) snapshot.picker_highlight_id = std::string(*active->highlighted_id());
        for (const auto index : active->filtered) snapshot.picker_visible_ids.push_back(active->items[index].id);
        snapshot.picker_loading = active->loading;
        snapshot.picker_error = active->error;
    } else if (selectable_list && selectable_list->query_enabled) {
        snapshot.picker_query = selectable_list->query;
        snapshot.picker_query_cursor = selectable_list->query_cursor;
        if (selectable_list->selected_id()) snapshot.picker_highlight_id = std::string(*selectable_list->selected_id());
        for (const auto &page : selectable_list->pages) {
            for (const auto &item : page.items) snapshot.picker_visible_ids.push_back(item.id);
        }
        snapshot.picker_loading = selectable_list->waiting_for_page || selectable_list->waiting_for_query;
        snapshot.picker_error = selectable_list->query_error ? selectable_list->query_error : selectable_list->page_error;
    }
    for (const auto &block : screen.transcript.blocks) {
        snapshot.blocks.push_back({.id = block.id,
                                   .kind = std::string(name(block.kind)),
                                   .state = std::string(name(block.state)),
                                   .text = block.text,
                                   .detail = block.detail,
                                   .command = block.command,
                                   .call_id = block.call_id,
                                   .output_item_id = block.output_item_id,
                                   .task_generation = block.activity_scope.task_generation,
                                   .provider_call_generation = block.activity_scope.provider_call_generation});
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
    auto frame = selectable_list ? selectable_list->frame(screen.size.columns, screen.size.rows) : screen.frame();
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
