#pragma once

#include <string>
#include <string_view>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

struct TerminalSize {
    i32 columns = 0;
    i32 rows = 0;

    friend bool operator==(const TerminalSize &, const TerminalSize &) = default;
};

enum class TerminalModifiers : u8 {
    NONE = 0,
    SHIFT = 1 << 0,
    ALT = 1 << 1,
    CONTROL = 1 << 2,
    SUPER = 1 << 3,
};

constexpr TerminalModifiers operator|(TerminalModifiers lhs, TerminalModifiers rhs) noexcept {
    return static_cast<TerminalModifiers>(static_cast<u8>(lhs) | static_cast<u8>(rhs));
}

constexpr bool has_modifier(TerminalModifiers value, TerminalModifiers modifier) noexcept {
    return (static_cast<u8>(value) & static_cast<u8>(modifier)) != 0;
}

enum class TerminalKey : u8 {
    UNKNOWN,
    CHARACTER,
    ENTER,
    BACKSPACE,
    TAB,
    ESCAPE,
    INSERT,
    DELETE_KEY,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
    ARROW_UP,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

enum class TerminalEventKind : u8 {
    KEY,
    TEXT,
    PASTE,
    RESIZE,
    FOCUS,
    MOUSE,
    CLOSED,
};

/// One normalized terminal event. Fields not relevant to `kind` are zeroed.
struct TerminalEvent {
    TerminalEventKind kind = TerminalEventKind::CLOSED;
    TerminalKey key = TerminalKey::UNKNOWN;
    TerminalModifiers modifiers = TerminalModifiers::NONE;
    std::string text;
    TerminalSize size;
    i32 repeat = 1;
    i32 x = 0;
    i32 y = 0;
    i32 mouse_buttons = 0;
    i32 wheel_delta = 0;
    bool pressed = true;
    bool focused = false;
};

/// Exclusive owner of an interactive terminal session.
///
/// The backend uses termios/poll on POSIX, native Console input on Windows,
/// and VT input when attached through ConPTY.
/// libuv participates only through EventLoop's thread-safe Relay wake bridge.
struct TerminalSession {
    struct Options {
        bool focus_events = false;
        bool mouse_events = false;

        constexpr Options(bool focus_events = false, bool mouse_events = false) : focus_events(focus_events), mouse_events(mouse_events) {}
    };

    TerminalSession() noexcept;

    TerminalSession(const TerminalSession &) = delete;
    TerminalSession &operator=(const TerminalSession &) = delete;

    TerminalSession(TerminalSession &&other) noexcept;
    TerminalSession &operator=(TerminalSession &&other) noexcept;

    ~TerminalSession();

    struct Self;
    Self *operator->() noexcept;

    /// Opens and activates the process's interactive alternate-screen
    /// session. Only one active session is allowed per process. File
    /// descriptors default to stdin/stdout.
    static Result<TerminalSession> open(i32 input_fd = 0, i32 output_fd = 1, Options options = Options(),
                                        EventLoop &loop = EventLoop::current());

    /// Native terminal detection.
    static bool attached(i32 fd) noexcept;

    Task<TerminalEvent, Error> next_event();

    Result<TerminalSize> size() const;

    /// Writes UTF-8 text and VT control sequences to the terminal.
    Error write(std::string_view bytes);

    /// Leaves the alternate screen, restores captured terminal modes, and
    /// pauses input delivery.
    Error suspend();

    /// Re-enters the alternate screen, reapplies requested modes, and resumes
    /// input delivery.
    Error resume();

    bool active() const noexcept;

private:
    explicit TerminalSession(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

} // namespace lighter
