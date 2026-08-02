#pragma once

#include <initializer_list>
#include <span>

#include <lighter/types.hpp>
#include <lighter/async/io/control.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/cancellation.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

/// Turns "the user wants us to stop" into Cancellation.
///
/// A long-running program has two different reactions to process-control
/// events. Some mean stop and some are merely observable. Fatal events fire a
/// CancellationToken, while every event also stays observable through next().
///
/// Typical use:
///
///   EventLoop loop;
///   auto interrupts = InterruptSource::create(loop).value();
///   loop.schedule(with_token(run_agent_turn(), interrupts.token()));
///
/// Cancellation is sticky: once a fatal signal has arrived the token stays
/// cancelled, and Tasks wrapped afterwards cancel immediately. This is
/// deliberate - a second turn should not start after the user asked to quit.
///
/// The second Ctrl+C is special. The first should unwind cleanly; if cleanup
/// itself wedges, the user needs a way out that does not involve a process
/// killer. interrupt_count() reports how many fatal signals have arrived so a
/// caller can escalate:
///
///   if (interrupts.interrupt_count() >= 2) std::exit(130);
///
/// Platform note: Windows console controls are represented directly rather
/// than disguised as POSIX signals. Terminal resize is a TerminalEvent, not a
/// process interrupt.
struct InterruptSource {
    InterruptSource() noexcept;

    InterruptSource(const InterruptSource &) = delete;
    InterruptSource &operator=(const InterruptSource &) = delete;

    InterruptSource(InterruptSource &&other) noexcept;
    InterruptSource &operator=(InterruptSource &&other) noexcept;

    ~InterruptSource();

    struct Self;
    Self *operator->() noexcept;

    /// Watches the default fatal set - INT, TERM and HUP - skipping any this
    /// platform cannot deliver.
    static Result<InterruptSource> create(EventLoop &loop = EventLoop::current());

    /// Watches `fatal` plus `observed`. Fatal controls cancel the token;
    /// observed controls only surface through next(). Unsupported controls are
    /// skipped so the same ideal set can be named on every platform.
    static Result<InterruptSource> create(std::span<const ControlEventKind> fatal, std::span<const ControlEventKind> observed,
                                          EventLoop &loop = EventLoop::current());

    static Result<InterruptSource> create(std::initializer_list<ControlEventKind> fatal, std::initializer_list<ControlEventKind> observed,
                                          EventLoop &loop = EventLoop::current());

    /// Fires when the first fatal signal arrives. Safe to copy and to hand to
    /// with_token().
    CancellationToken token() const noexcept pre(self != nullptr);

    /// Resolves with the next signal of any kind, fatal ones included.
    ///
    /// At most one pending notification is retained per signal kind; repeated
    /// deliveries of the same kind are coalesced until it is consumed. Fatal
    /// deliveries are still counted individually by interrupt_count().
    ///
    /// The InterruptSource must outlive every Task returned by next(). In the
    /// usual setup it lives for the loop's whole run, with EventLoop outliving
    /// the source. Single-consumer: overlapping calls fail with
    /// k_connection_already_in_progress.
    Task<ControlEventKind, Error> next();

    /// Number of fatal signals delivered so far. A value of 2 or more means the
    /// user asked twice and the caller should consider exiting hard.
    i32 interrupt_count() const noexcept;

    /// True once a fatal signal has arrived, i.e. token() has fired.
    bool interrupted() const noexcept;

private:
    explicit InterruptSource(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

} // namespace lighter
