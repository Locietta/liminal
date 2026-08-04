#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <lighter/async/io/terminal.h>
#include <lighter/types.hpp>

#include "liminal/event.h"
#include "liminal/tui/surface.h"
#include "liminal/tui/transcript.h"

namespace liminal::tui {

using namespace lighter::types;

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
    std::vector<LayoutRow> rows;
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
    void resize(lighter::TerminalSize next) noexcept;
    void set_model(std::string_view name, const std::optional<std::string> &effort);
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
    void move_home();
    void move_end();
    void move_document_home();
    void move_document_end();
    void clear_prompt();
    std::string take_prompt();

    void scroll(i32 rows);
    void page(i32 direction);
    void follow_tail() noexcept;

    Frame frame() const;
    i32 viewport_rows() const;
    std::vector<LayoutRow> layout_block(const Block &block) const;
    LayoutDiagnostics layout_diagnostics() const noexcept;

    lighter::TerminalSize size{80, 24};
    Transcript transcript;
    Composer composer;
    PromptHistory prompt_history;
    std::string model;
    std::optional<std::string> effort;
    std::optional<ViewportAnchor> anchor;
    bool unread = false;
    SessionState state = SessionState::EDITING;
    mutable std::unordered_map<u64, CachedBlockLayout> layout_cache;
    mutable LayoutDiagnostics diagnostics;

private:
    void mark_editing() noexcept;
};

} // namespace liminal::tui
