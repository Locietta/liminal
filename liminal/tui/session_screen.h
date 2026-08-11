#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <lighter/async/io/terminal.h>
#include <lighter/types.hpp>

#include <liminal/event.h>
#include <liminal/tui/surface.h>
#include <liminal/tui/transcript.h>

namespace liminal::tui {

using namespace lighter::types;

using MonotonicNow = std::copyable_function<std::chrono::steady_clock::time_point() const>;

struct Composer {
    void insert(std::string_view value);
    void backspace();
    void erase();
    void backspace_word();
    void erase_word();
    void move_left();
    void move_right();
    void move_word_left();
    void move_word_right();
    bool move_up();
    bool move_down();
    void move_home();
    void move_end();
    void move_document_home() noexcept;
    void move_document_end() noexcept;
    void replace(std::string value);
    void clear() noexcept;
    std::string take();

    bool empty() const noexcept { return text.empty(); }

    std::string text;
    usize cursor = 0;
    std::optional<i32> preferred_column;
};

struct PromptHistory {
    void record(const std::string &prompt);
    bool previous(Composer &composer);
    bool next(Composer &composer);
    void edited() noexcept;

    std::vector<std::string> entries;
    std::optional<usize> index;
    std::string draft;
};

struct ViewportAnchor {
    u64 block_id = 0;
    usize source_offset = 0;

    friend bool operator==(const ViewportAnchor &, const ViewportAnchor &) = default;
};

struct CellPoint {
    i32 row = 0;
    i32 column = 0;

    friend auto operator<=>(const CellPoint &, const CellPoint &) = default;
};

/// Mouse drag selection over screen cells, in reading order between the press
/// anchor and the drag focus. Cell coordinates deliberately mirror native
/// terminal selection: content may stream underneath an active drag.
struct SelectionState {
    CellPoint anchor;
    CellPoint focus;
};

struct LayoutRow {
    u64 block_id = 0;
    usize source_offset = 0;
    std::string text;
    Style style = Style::NORMAL;
    std::vector<StyledSpan> spans;
};

struct LayoutDiagnostics {
    u64 cache_hits = 0;
    u64 cache_misses = 0;
    u64 blocks_laid_out = 0;
    usize cached_blocks = 0;

    friend bool operator==(const LayoutDiagnostics &, const LayoutDiagnostics &) = default;
};

struct CachedBlockLayout {
    i32 columns = 0;
    BlockKind kind = BlockKind::NOTICE;
    BlockState state = BlockState::COMPLETED;
    std::string text;
    std::string detail;
    std::string tool_name;
    std::string command;
    std::vector<LayoutRow> rows;
};

struct SessionFooter {
    std::string workspace_path = ".";
    std::optional<u32> context_left_percent;
    u64 tokens_used = 0;
};

enum struct SessionState {
    EDITING,
    WAITING,
    STREAMING,
    RUNNING_TOOLS,
    COMPLETED,
    CANCELLED,
    FAILED,
};

/// Application-owned TUI state. Transcript and composer source data are
/// independent of terminal width; Frame is a disposable projection.
struct SessionScreen {
    explicit SessionScreen(MonotonicNow now = {});

    void resize(lighter::TerminalSize next) noexcept;
    void set_model(std::string_view name, const std::optional<std::string> &effort);
    void set_footer(SessionFooter next);
    void show_status(std::string text);
    void apply(const Event &event);
    void add_notice(std::string text);

    void insert(std::string_view text);
    void backspace();
    void erase();
    void backspace_word();
    void erase_word();
    void move_left();
    void move_right();
    void move_word_left();
    void move_word_right();
    void move_up();
    void move_down();
    void previous_prompt();
    void next_prompt();
    void move_home();
    void move_end();
    void move_document_home();
    void move_document_end();
    void replace_prompt(std::string text);
    void clear_prompt();
    std::string take_prompt();

    void scroll(i32 rows);
    void page(i32 direction);
    void follow_tail() noexcept;

    void begin_selection(i32 row, i32 column);
    bool extend_selection(i32 row, i32 column);
    /// Returns the selected text and clears the selection. A click that never
    /// moved off its press cell returns an empty string.
    std::string take_selection();

    Frame frame() const;
    i32 viewport_rows() const;
    std::vector<LayoutRow> layout_block(const Block &block) const;
    LayoutDiagnostics layout_diagnostics() const noexcept;
    bool has_elapsed_running_command() const;
    bool working() const noexcept;
    bool animating() const;
    std::vector<LayoutRow> working_rows() const;

    lighter::TerminalSize size{80, 24};
    Transcript transcript;
    Composer composer;
    PromptHistory prompt_history;
    std::string model;
    std::optional<std::string> effort;
    SessionFooter footer;
    std::optional<std::string> transient_status;
    std::optional<ViewportAnchor> anchor;
    std::optional<SelectionState> selection;
    bool unread = false;
    bool external_editor_active = false;
    SessionState state = SessionState::EDITING;
    std::chrono::steady_clock::time_point turn_started_at;
    mutable std::unordered_map<u64, CachedBlockLayout> layout_cache;
    mutable LayoutDiagnostics diagnostics;

private:
    void mark_editing() noexcept;

    MonotonicNow monotonic_now;
};

} // namespace liminal::tui
