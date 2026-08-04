#pragma once

#include <optional>
#include <string>
#include <string_view>

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
    void move_left();
    void move_right();
    void move_home() noexcept;
    void move_end() noexcept;
    void clear() noexcept;
    std::string take();

    bool empty() const noexcept { return text.empty(); }

    std::string text;
    usize cursor = 0;
};

struct ViewportAnchor {
    u64 block_id = 0;
    usize source_offset = 0;

    friend bool operator==(const ViewportAnchor &, const ViewportAnchor &) = default;
};

/// Phase 0 application-owned TUI state. Transcript and composer source data
/// are independent of terminal width; Frame is a disposable projection.
struct SessionScreen {
    void resize(lighter::TerminalSize next) noexcept;
    void set_model(std::string_view name, const std::optional<std::string> &effort);
    void apply(const Event &event);
    void add_notice(std::string text);

    void scroll(i32 rows);
    void page(i32 direction);
    void follow_tail() noexcept;

    Frame frame() const;
    i32 viewport_rows() const noexcept;

    lighter::TerminalSize size{80, 24};
    Transcript transcript;
    Composer composer;
    std::string model;
    std::optional<std::string> effort;
    std::optional<ViewportAnchor> anchor;
    bool unread = false;
};

} // namespace liminal::tui
