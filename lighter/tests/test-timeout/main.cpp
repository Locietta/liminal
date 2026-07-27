#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;
using namespace std::chrono_literals;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

/// Finishes well inside any budget used below.
Task<i32, Error> quick(i32 value) {
    co_await sleep(1ms);
    co_return value;
}

/// Runs far longer than any budget used below. `finished` stays false unless
/// the Task was allowed to run to completion, which is how the tests detect a
/// Task that outlived its timeout.
Task<i32, Error> slow(bool &finished) {
    co_await sleep(2s);
    finished = true;
    co_return 7;
}

Task<i32, Error> fails_fast() {
    co_await sleep(1ms);
    co_await fail(Error::k_permission_denied);
    co_return 0;
}

/// The wrapped Task beats the budget: its value is forwarded untouched.
Task<void, Error> value_wins(bool &ok) {
    auto result = co_await with_timeout(quick(42), 1s);
    ok = result.has_value() && *result == 42;
}

/// The budget expires first: the result is a timeout Error and the wrapped Task
/// must have been cancelled rather than left running.
Task<void, Error> timeout_wins(bool &ok, bool &inner_finished) {
    auto result = co_await with_timeout(slow(inner_finished), 10ms);
    ok = result.has_error() && result.error() == Error::k_connection_timed_out;
}

/// A structured Error outranks the timeout and is forwarded as-is.
Task<void, Error> error_beats_timeout(bool &ok) {
    auto result = co_await with_timeout(fails_fast(), 1s);
    ok = result.has_error() && result.error() == Error::k_permission_denied;
}

/// An outer token cancelling the race must cancel, not time out: the two must
/// stay distinguishable at the call site.
Task<void, Error> outer_cancel_wins(bool &ok) {
    CancellationSource source;

    auto canceller = [](CancellationSource &source) -> Task<void, Error> {
        co_await sleep(5ms);
        source.cancel();
    };

    bool inner_finished = false;
    auto guarded = with_token(with_timeout(slow(inner_finished), 5s), source.token());
    auto result = co_await WhenAll(std::move(guarded), canceller(source));

    ok = result.is_cancelled();
}

/// A zero budget still fails with a timeout rather than hanging.
Task<void, Error> zero_budget(bool &ok) {
    bool inner_finished = false;
    auto result = co_await with_timeout(slow(inner_finished), 0ms);
    ok = result.has_error() && result.error() == Error::k_connection_timed_out;
}

/// A deadline already in the past behaves like a zero budget.
Task<void, Error> past_deadline(bool &ok) {
    bool inner_finished = false;
    auto result = co_await with_deadline(slow(inner_finished), std::chrono::steady_clock::now() - 1s);
    ok = result.has_error() && result.error() == Error::k_connection_timed_out;
}

/// A negative budget - what a caller gets from subtracting an already-passed
/// deadline - must time out promptly. Unclamped it would abort on Timer::start's
/// assertion in debug, or cast to a huge u64 and hang in release.
Task<void, Error> negative_budget(bool &ok) {
    bool inner_finished = false;
    auto result = co_await with_timeout(slow(inner_finished), -5s);
    ok = result.has_error() && result.error() == Error::k_connection_timed_out;
}

/// The deadline is absolute, so the budget must be measured when the wrapper
/// starts running - not when it was constructed.
///
/// Tasks are lazy. Building the wrapper and awaiting it later used to bank the
/// full remaining budget from construction time, letting the work run well past
/// the deadline the caller actually asked for. Here the deadline expires while
/// the wrapper is still sitting unstarted, so it must time out immediately.
Task<void, Error> deadline_measured_at_start(bool &ok) {
    bool inner_finished = false;
    auto guarded = with_deadline(slow(inner_finished), std::chrono::steady_clock::now() + 20ms);

    // Let the deadline pass before the wrapper ever runs.
    co_await sleep(50ms);

    // Timed, not just checked for the error: sampling at construction still
    // ends in a timeout, only ~20ms too late. Elapsed time is what separates
    // "honoured the absolute deadline" from "restarted the clock on resume".
    const auto start = std::chrono::steady_clock::now();
    auto result = co_await std::move(guarded);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    ok = result.has_error() && result.error() == Error::k_connection_timed_out && waited < 10ms;
}

/// A void-valued Task round-trips through with_timeout().
Task<void, Error> void_value(bool &ok) {
    auto body = []() -> Task<void, Error> { co_await sleep(1ms); };
    auto result = co_await with_timeout(body(), 1s);
    ok = result.has_value();
}

i32 run_all() {
    EventLoop loop;

    bool value_ok = false;
    bool timeout_ok = false;
    bool inner_finished = false;
    bool error_ok = false;
    bool cancel_ok = false;
    bool zero_ok = false;
    bool deadline_ok = false;
    bool negative_ok = false;
    bool lazy_deadline_ok = false;
    bool void_ok = false;

    loop.schedule(value_wins(value_ok));
    loop.schedule(timeout_wins(timeout_ok, inner_finished));
    loop.schedule(error_beats_timeout(error_ok));
    loop.schedule(outer_cancel_wins(cancel_ok));
    loop.schedule(zero_budget(zero_ok));
    loop.schedule(past_deadline(deadline_ok));
    loop.schedule(negative_budget(negative_ok));
    loop.schedule(deadline_measured_at_start(lazy_deadline_ok));
    loop.schedule(void_value(void_ok));
    loop.run();

    require(value_ok, "a Task that beats its budget must forward its value");
    require(timeout_ok, "an expired budget must fail with k_connection_timed_out");
    require(!inner_finished, "a timed-out Task must be cancelled, not left running");
    require(error_ok, "a structured Error must outrank the timeout");
    require(cancel_ok, "an outer token must cancel rather than time out");
    require(zero_ok, "a zero budget must time out");
    require(deadline_ok, "a deadline in the past must time out");
    require(negative_ok, "a negative budget must time out rather than hang");
    require(lazy_deadline_ok, "a deadline must be measured when the wrapper starts, not when it is built");
    require(void_ok, "a void-valued Task must round-trip through with_timeout");

    return 0;
}

} // namespace

int main() {
    try {
        return run_all();
    } catch (const std::exception &e) {
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
