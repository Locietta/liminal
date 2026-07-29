#pragma once

#include <initializer_list>
#include <span>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/async/io/watcher.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/cancellation.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

/// Turns "the user wants us to stop" into Cancellation.
///
/// A long-running program has two different reactions to a signal. Some signals
/// mean stop - the whole Task tree should unwind - and some are just news, like
/// a terminal resize. InterruptSource handles both without making callers poll:
/// fatal signals fire a CancellationToken, so every Task already wrapped in
/// with_token() unwinds through the ordinary Cancellation path, while every
/// signal (fatal or not) also stays observable through next().
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
/// Platform note: on Windows the deliverable set is Ctrl+C, Ctrl+Break,
/// console-close and an emulated resize - see SignalKind. TERM is silently
/// absent there, and Task Manager's "End task" cannot be observed by any means.
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

    /// Watches `fatal` plus `observed`. Signals in `fatal` cancel the token;
    /// signals in `observed` only surface through next(), which is how a
    /// resize (WINCH) is watched without tearing the program down.
    /// Unsupported signals are skipped rather than rejected, so the same call
    /// compiles and runs on every platform.
    static Result<InterruptSource> create(std::span<const SignalKind> fatal, std::span<const SignalKind> observed,
                                          EventLoop &loop = EventLoop::current());

    static Result<InterruptSource> create(std::initializer_list<SignalKind> fatal, std::initializer_list<SignalKind> observed,
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
    Task<SignalKind, Error> next();

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
