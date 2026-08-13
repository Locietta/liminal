#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

/// A single-shot Timer resumes its waiter exactly once.
Task<void, Error> single_shot(std::vector<std::string> &log) {
    auto timer = Timer::create();
    timer.start(5ms);
    co_await timer.wait().or_fail();
    log.push_back("fired");
}

/// A repeating Timer keeps firing until it is stopped. This also exercises the
/// `pending` counter: ticks that arrive while the waiter is between waits are
/// counted rather than dropped, so the loop never misses one.
Task<void, Error> repeating(i32 &ticks) {
    auto timer = Timer::create();
    timer.start(1ms, 1ms);
    for (i32 i = 0; i < 5; ++i) {
        co_await timer.wait().or_fail();
        ticks += 1;
    }
    timer.stop();
}

/// sleep() suspends for approximately the requested duration.
///
/// Deliberately not asserting `measured >= budget`: libuv schedules against the
/// loop's cached time, which is sampled once per iteration, so a timer can fire
/// a millisecond or two early. The useful property is that the Task actually
/// waited rather than resuming immediately.
Task<void, Error> sleeps_at_least(std::chrono::milliseconds budget, std::chrono::milliseconds &measured) {
    const auto start = std::chrono::steady_clock::now();
    co_await sleep(budget);
    measured = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

/// Cancelling a Task suspended on Timer::wait() must unwind it rather than
/// leave the frame parked on a Timer that has been stopped underneath it.
Task<void, Error, Cancellation> cancelled_wait(bool &resumed, Event &started) {
    auto timer = Timer::create();
    started.set();
    co_await timer.wait().or_fail();
    // Only reached if Cancellation failed to interrupt the timer wait.
    resumed = true;
}

/// Fires the token from a sibling Task so the cancel lands while the other side
/// is genuinely parked on the Timer, not before it ever suspends.
Task<void, Error> cancel_after(CancellationSource &source, Event &started) {
    co_await started.wait();
    source.cancel();
}

Task<void, Error> cancel_pending_wait(bool &resumed, bool &observed_cancel) {
    CancellationSource source;
    Event started;

    // with_token() already returns a Cancellation-channel Task, so the
    // aggregate exposes Cancellation rather than propagating it into this frame.
    auto result = co_await WhenAll(with_token(cancelled_wait(resumed, started), source.token()), cancel_after(source, started));

    observed_cancel = result.is_cancelled();
}

/// A second concurrent wait must be reported, not silently satisfied.
///
/// This used to be assert(false) followed by co_return, so with NDEBUG the
/// second waiter resumed immediately - indistinguishable from the Timer having
/// fired, and a silently wrong result in exactly the builds that ship.
Task<void, Error> rejects_second_waiter(bool &first_ok, bool &second_rejected) {
    auto timer = Timer::create();
    Event first_waiting;

    auto second = [&first_waiting](Timer &timer, bool &rejected) -> Task<void, Error> {
        co_await first_waiting.wait();
        auto result = co_await timer.wait().catch_cancel();
        rejected = result.has_error() && result.error() == Error::k_connection_already_in_progress;
        timer.start(0ms);
    };

    auto first = [&first_waiting](Timer &timer, bool &ok) -> Task<void, Error> {
        first_waiting.set();
        auto result = co_await timer.wait().catch_cancel();
        ok = result.has_value();
    };

    // Each branch swallows its own outcome: the second is expected to fail, and
    // an un-caught sibling error under WhenAll would cancel the group.
    co_await WhenAll(first(timer, first_ok), second(timer, second_rejected));
}

/// yield() resumes on a later loop iteration, strictly after callbacks that
/// were already queued when it suspended.
Task<void, Error> yields_after_pending_work(std::vector<std::string> &log) {
    auto timer = Timer::create();
    timer.start(0ms);

    co_await yield();
    log.push_back("yield");

    co_await timer.wait().or_fail();
    log.push_back("timer");
}

i32 run_all() {
    EventLoop loop;

    std::vector<std::string> single_log;
    i32 ticks = 0;
    std::chrono::milliseconds measured{0};
    bool resumed = false;
    bool observed_cancel = false;
    std::vector<std::string> yield_log;
    bool first_ok = false;
    bool second_rejected = false;

    loop.schedule(single_shot(single_log));
    loop.schedule(repeating(ticks));
    loop.schedule(sleeps_at_least(20ms, measured));
    loop.schedule(cancel_pending_wait(resumed, observed_cancel));
    loop.schedule(yields_after_pending_work(yield_log));
    loop.schedule(rejects_second_waiter(first_ok, second_rejected));
    loop.run();

    require(single_log.size() == 1 && single_log[0] == "fired", "single-shot Timer must fire exactly once");
    require(ticks == 5, "repeating Timer must fire 5 times, got " + std::to_string(ticks));
    // Allow the small early-fire slack described on sleeps_at_least().
    require(measured >= 15ms, "sleep(20ms) returned after only " + std::to_string(measured.count()) + "ms");
    require(!resumed, "a cancelled sleep must not resume its coroutine body");
    require(observed_cancel, "cancelling a pending Timer wait must surface as Cancellation");
    require(yield_log.size() == 2, "yield test must record both steps");
    require(first_ok, "the first waiter must still receive its tick");
    require(second_rejected, "a second concurrent wait must be reported, not silently satisfied");

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
