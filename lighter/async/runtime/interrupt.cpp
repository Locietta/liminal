#include "interrupt.h"

#include <algorithm>
#include <contracts>
#include <deque>
#include <utility>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/async/io/control.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/vocab/cancellation.h>

namespace lighter {

struct InterruptSource::Self {
    ControlEventSource controls;
    CancellationSource source;
    std::vector<ControlEventKind> fatal;
    i32 interrupts = 0;

    /// The pump is the only consumer of `controls`; it re-queues everything it
    /// sees here so next() has something to read without competing with it for
    /// the set's single waiter slot.
    std::deque<ControlEventKind> observed;
    Event ready;

    /// Guards `observed` against a second concurrent next(). Event wakes every
    /// waiter, but only one can take the queued signal, so without this the
    /// losers would silently loop instead of getting the documented error.
    bool reading = false;

    /// Kept alive for as long as this object, so that destroying the source
    /// tears the pump down instead of leaving it parked on a control source that is
    /// about to disappear.
    Task<void, Error, Cancellation> pump;

    bool is_fatal(ControlEventKind kind) const noexcept { return std::ranges::find(fatal, kind) != fatal.end(); }

    static void destroy(Self *self) noexcept {
        if (!self) {
            return;
        }

        // Cancel before any member dies: this runs the pump awaiter's cancel
        // hook, which disarms it from the control source. Dropping the frame while
        // it was still armed would leave a dangling waiter behind.
        if (auto *node = self->pump.operator->()) {
            node->cancel();
        }

        delete self;
    }
};

namespace {

/// Ideal fatal set; unsupported controls are filtered per platform.
constexpr ControlEventKind k_default_fatal[] = {
    ControlEventKind::INTERRUPT,
    ControlEventKind::TERMINATE,
    ControlEventKind::HANGUP,
    ControlEventKind::BREAK,
};

/// Drops controls this platform cannot deliver, so callers can name the ideal
/// set once instead of writing #ifdefs at every call site. Also deduplicates.
std::vector<ControlEventKind> supported_only(std::span<const ControlEventKind> kinds) {
    std::vector<ControlEventKind> out;
    out.reserve(kinds.size());
    for (auto kind : kinds) {
        if (control_event_supported(kind) && std::ranges::find(out, kind) == out.end()) {
            out.push_back(kind);
        }
    }
    return out;
}

/// Sole consumer of the control source: cancels the token on the first fatal
/// event and republishes every event for next().
///
/// Runs for the lifetime of the source. It is cancelled by Self::destroy.
Task<void, Error, Cancellation> run_pump(InterruptSource::Self *self) {
    while (true) {
        // Background wait: this pump runs for the whole life of the source, so
        // if it held the loop open the process could never shut down normally -
        // run() would sit here forever waiting for a control that may never come.
        auto signalled = co_await self->controls.next_background().catch_cancel();
        if (!signalled.has_value()) {
            // Cancelled (teardown) or the set failed - either way, stop.
            co_return;
        }

        const auto kind = *signalled;
        if (self->is_fatal(kind)) {
            self->interrupts += 1;
            // Sticky: later Tasks wrapped in this token cancel immediately, so
            // no new work starts after the user asked to quit.
            self->source.cancel();
        }

        if (std::ranges::find(self->observed, kind) == self->observed.end()) {
            self->observed.push_back(kind);
            self->ready.set();
        }
    }
}

} // namespace

InterruptSource::InterruptSource() noexcept = default;

InterruptSource::InterruptSource(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

InterruptSource::~InterruptSource() = default;

InterruptSource::InterruptSource(InterruptSource &&other) noexcept = default;

InterruptSource &InterruptSource::operator=(InterruptSource &&other) noexcept = default;

InterruptSource::Self *InterruptSource::operator->() noexcept { return self.get(); }

Result<InterruptSource> InterruptSource::create(EventLoop &loop) { return create(k_default_fatal, {}, loop); }

Result<InterruptSource> InterruptSource::create(std::span<const ControlEventKind> fatal, std::span<const ControlEventKind> observed,
                                                EventLoop &loop) {
    auto fatal_kinds = supported_only(fatal);

    auto watched = fatal_kinds;
    for (auto kind : supported_only(observed)) {
        if (std::ranges::find(watched, kind) == watched.end()) {
            watched.push_back(kind);
        }
    }

    if (watched.empty()) {
        return outcome_error(Error::k_invalid_argument);
    }

    auto controls = ControlEventSource::create(watched, loop);
    if (!controls) {
        return outcome_error(controls.error());
    }

    auto self = UniqueHandle<Self>(new Self());
    self->controls = std::move(*controls);
    self->fatal = std::move(fatal_kinds);

    // Start the pump only once Self sits at its final address. Starting it
    // eagerly also ensures destruction can cancel a suspended Task instead of
    // destroying a frame still queued inside EventLoop.
    self->pump = run_pump(self.get());
    loop.start(*self->pump.operator->());

    return InterruptSource(std::move(self));
}

Result<InterruptSource> InterruptSource::create(std::initializer_list<ControlEventKind> fatal,
                                                std::initializer_list<ControlEventKind> observed, EventLoop &loop) {
    return create(std::span<const ControlEventKind>(fatal.begin(), fatal.size()),
                  std::span<const ControlEventKind>(observed.begin(), observed.size()), loop);
}

CancellationToken InterruptSource::token() const noexcept { return self->source.token(); }

Task<ControlEventKind, Error> InterruptSource::next() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    // Take a queued control without blocking. Checked before the single-consumer
    // guard, mirroring ControlEventSource::next(): a backlog can be drained by any
    // number of sequential callers, and only actually parking is exclusive.
    if (self->observed.empty()) {
        // Single-consumer while suspended: Event wakes every waiter, but only
        // one can take the signal, so a second sleeper would loop back and hang
        // until some later, unrelated signal arrived. Report that instead.
        if (self->reading) {
            co_await fail(Error::k_connection_already_in_progress);
        }

        self->reading = true;

        // The caller is blocking on this, so it is the program's work: hold the
        // loop open for it. The pump's own wait is deliberately background and
        // would not keep us up, and an Event carries no uv reference of its
        // own, so without this the loop could exit with the caller parked here.
        self->controls.hold_loop();

        while (self->observed.empty()) {
            auto woken = co_await self->ready.wait().catch_cancel();
            if (woken.is_cancelled()) {
                // Release the slot before unwinding, or the source is deaf to
                // every later next() call.
                self->controls.release_loop();
                self->reading = false;
                co_await cancel();
            }
        }

        self->controls.release_loop();
        self->reading = false;
    }

    auto kind = self->observed.front();
    self->observed.pop_front();

    // Re-arm only once the backlog is drained, so a signal that lands between
    // the pop and the next call is not lost.
    if (self->observed.empty()) {
        self->ready.reset();
    }

    co_return kind;
}

i32 InterruptSource::interrupt_count() const noexcept { return self ? self->interrupts : 0; }

bool InterruptSource::interrupted() const noexcept { return self && self->source.cancelled(); }

} // namespace lighter
