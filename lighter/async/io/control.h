#pragma once

#include <initializer_list>
#include <span>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

/// Semantic process-control events. These intentionally do not pretend that
/// Windows console controls are POSIX signals.
enum class ControlEventKind : u8 {
    INTERRUPT,
    TERMINATE,
    HANGUP,
    QUIT,
    BREAK,
    LOGOFF,
    SHUTDOWN,
    SUSPEND,
};

/// True when the current platform can deliver this control event.
bool control_event_supported(ControlEventKind kind) noexcept;

/// Native process-control event source integrated with EventLoop.
///
/// POSIX uses sigaction plus an async-signal-safe self-pipe. Windows uses
/// SetConsoleCtrlHandler plus kernel events. Native worker threads only wait
/// and normalize events; Relay transfers delivery to the Event-loop thread.
struct ControlEventSource {
    ControlEventSource() noexcept;

    ControlEventSource(const ControlEventSource &) = delete;
    ControlEventSource &operator=(const ControlEventSource &) = delete;

    ControlEventSource(ControlEventSource &&other) noexcept;
    ControlEventSource &operator=(ControlEventSource &&other) noexcept;

    ~ControlEventSource();

    struct Self;
    Self *operator->() noexcept;

    /// Creates a source for the exact supported set. Empty sets, unsupported
    /// events, and duplicate process-wide registrations are rejected.
    static Result<ControlEventSource> create(std::span<const ControlEventKind> kinds, EventLoop &loop = EventLoop::current());

    static Result<ControlEventSource> create(std::initializer_list<ControlEventKind> kinds, EventLoop &loop = EventLoop::current());

    /// Waits for one event and keeps the loop alive while suspended.
    Task<ControlEventKind, Error> next();

    /// Waits without keeping the loop alive. Intended for lifetime supervisors
    /// such as InterruptSource whose presence must not prevent clean shutdown.
    Task<ControlEventKind, Error> next_background();

    void hold_loop() noexcept;
    void release_loop() noexcept;
    bool holding_loop() const noexcept;

private:
    explicit ControlEventSource(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

} // namespace lighter
