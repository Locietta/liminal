#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <lighter/types.hpp>

#include "liminal/tui/session_screen.h"

namespace liminal::tui {

using namespace lighter::types;

struct HeadlessAction {
    std::string type;
    std::string text;
    std::string call_id;
    std::string name;
    std::optional<std::string> effort;
    i32 columns = 0;
    i32 rows = 0;
    i32 amount = 0;
    i64 milliseconds = 0;
    bool is_error = false;
};

struct SnapshotBlock {
    u64 id = 0;
    std::string kind;
    std::string state;
    std::string text;
    std::string call_id;
};

struct SnapshotAnchor {
    u64 block_id = 0;
    usize source_offset = 0;
};

struct SnapshotCursor {
    i32 row = 0;
    i32 column = 0;
    bool visible = false;
};

struct SnapshotCell {
    i32 row = 0;
    i32 column = 0;
    std::string text;
    std::string style;
    bool continuation = false;
};

struct HeadlessSnapshot {
    std::string schema_version = "1";
    std::string driver = "tui.headless";
    i64 now_ms = 0;
    bool render_pending = false;
    u64 render_count = 0;
    u64 action_count = 0;
    usize text_bytes = 0;
    i32 columns = 0;
    i32 rows = 0;
    std::string model;
    std::string semantic_state;
    std::optional<std::string> effort;
    std::string composer_text;
    usize composer_cursor = 0;
    std::optional<SnapshotAnchor> anchor;
    bool unread = false;
    std::vector<SnapshotBlock> blocks;
    std::vector<std::string> visible_text;
    SnapshotCursor cursor;
    std::vector<SnapshotCell> cells;
    LayoutDiagnostics layout;
    std::vector<std::string> ansi_operations;
};

/// Deterministic adapter over the production reducer and frame projector.
/// Time advances only through actions and no external capabilities are held.
struct HeadlessSession {
    explicit HeadlessSession(i32 columns = 80, i32 rows = 24, i64 now_ms = 0);

    std::expected<void, std::string> apply(const HeadlessAction &action);
    std::expected<void, std::string> apply(const std::vector<HeadlessAction> &actions);
    HeadlessSnapshot inspect() const;

    SessionScreen screen;
    i64 now_ms = 0;
    bool render_pending = false;
    i64 render_at_ms = 0;
    u64 render_count = 0;
    u64 action_count = 0;
    usize text_bytes = 0;
    std::optional<Frame> previous_frame;
    std::vector<std::string> ansi_operations;

private:
    void invalidate();
    void flush();
};

} // namespace liminal::tui
