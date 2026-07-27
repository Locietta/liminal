#pragma once

#include <chrono>
#include <concepts>
#include <type_traits>
#include <utility>
#include <variant>

#include <lighter/async/io/loop.h>
#include <lighter/async/io/watcher.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

namespace detail {

/// Index of the Timer inside the WhenAny used by with_timeout(). The wrapped
/// Task is armed first, so the Timer always occupies the second alternative.
constexpr inline usize k_timeout_timer_index = 1;

} // namespace detail

/// Runs `task` with an upper bound on how long it may take.
///
/// Races the Task against a Timer. Whichever settles first cancels the other,
/// so exactly one of these happens:
///   1. the Task finishes in time - its value (or Error) is forwarded;
///   2. the budget expires - the Task is cancelled and the returned Task fails
///      with Error::k_connection_timed_out;
///   3. the Task cancels itself, or an outer token cancels the race - the
///      returned Task cancels, and the Timer is torn down.
///
/// The timeout is reported on the Error channel rather than as Cancellation:
/// "we gave up waiting" is a different outcome from "someone cancelled us", and
/// only the Error channel can carry that distinction (Task's cancel channel is
/// restricted to a valueless Cancellation).
///
/// Structured completion still holds - WhenAny does not settle until both the
/// Task and the Timer have reached a terminal state - so the Task is never left
/// running past the point where this Task resumes its caller.
template <typename T, typename E, typename C>
    requires(std::is_void_v<E> || std::constructible_from<Error, E>)
Task<T, Error, Cancellation> with_timeout(Task<T, E, C> task, std::chrono::milliseconds budget, EventLoop &loop = EventLoop::current()) {
    // A caller that derives a budget by subtraction can hand us a negative one
    // once the deadline has passed. Timer::start asserts non-negative and
    // otherwise casts to u64, which would turn "already expired" into an
    // effectively infinite wait - clamp instead, so an expired budget times out
    // promptly rather than hanging.
    if (budget < std::chrono::milliseconds{0}) {
        budget = std::chrono::milliseconds{0};
    }

    // The wrapped Task is wrapped with catch_cancel() so that its Cancellation
    // surfaces on the aggregate's cancel channel instead of silently cancelling
    // this frame - we need to tell "the Task cancelled" apart from "the Timer
    // won" to pick the right outcome below.
    auto race = co_await WhenAny(std::move(task).catch_cancel(), sleep(budget, loop));

    if constexpr (!std::is_void_v<E>) {
        if (race.has_error()) {
            // A real Error outranks the timeout even if both landed in the same
            // turn: WhenAny records the Error and never reports CANCELLED once
            // one is present.
            co_await fail(Error(std::move(race).error()));
        }
    }

    // Guard value access with has_value() rather than relying on co_await
    // fail()/cancel() making the rest unreachable - MSVC's coroutine codegen can
    // fall through past a symmetric-transfer suspension. See the same guard in
    // with_token() (vocab/cancellation.h).
    if (race.has_value()) {
        auto &&winner = *race;
        if (winner.index() != detail::k_timeout_timer_index) {
            if constexpr (!std::is_void_v<T>) {
                co_return std::move(std::get<0>(winner));
            } else {
                co_return;
            }
        }

        co_await fail(Error::k_connection_timed_out);
    }

    co_await cancel();
}

/// with_timeout() against an absolute steady-clock deadline.
///
/// A deadline already in the past yields a zero budget, which still lets the
/// Task run until its first suspension point before the Timer can fire.
///
/// The remaining budget is measured when this Task starts, not when it is
/// constructed. Tasks are lazy, so a caller that builds the wrapper and
/// schedules it later would otherwise get a budget that started counting at
/// construction time and run well past the absolute deadline it asked for.
template <typename T, typename E, typename C>
    requires(std::is_void_v<E> || std::constructible_from<Error, E>)
Task<T, Error, Cancellation> with_deadline(Task<T, E, C> task, std::chrono::steady_clock::time_point deadline,
                                           EventLoop &loop = EventLoop::current()) {
    // Sampled here, inside the coroutine body, so it runs on first resume
    // rather than at construction.
    const auto now = std::chrono::steady_clock::now();
    const auto budget =
        deadline > now ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) : std::chrono::milliseconds{0};

    auto result = co_await with_timeout(std::move(task), budget, loop);

    if (result.has_error()) {
        co_await fail(std::move(result).error());
    }

    if (result.has_value()) {
        if constexpr (!std::is_void_v<T>) {
            co_return std::move(*result);
        } else {
            co_return;
        }
    }

    co_await cancel();
}

} // namespace lighter
