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
    co_await timer.wait();
    log.push_back("fired");
}

/// A repeating Timer keeps firing until it is stopped. This also exercises the
/// `pending` counter: ticks that arrive while the waiter is between waits are
/// counted rather than dropped, so the loop never misses one.
Task<void, Error> repeating(i32 &ticks) {
    auto timer = Timer::create();
    timer.start(1ms, 1ms);
    for (i32 i = 0; i < 5; ++i) {
        co_await timer.wait();
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
Task<void, Error, Cancellation> cancelled_wait(bool &resumed) {
    co_await sleep(1s);
    // Only reached if Cancellation failed to interrupt the sleep.
    resumed = true;
}

/// Fires the token from a sibling Task so the cancel lands while the other side
/// is genuinely parked on the Timer, not before it ever suspends.
Task<void, Error> cancel_after(CancellationSource &source, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    source.cancel();
}

Task<void, Error> cancel_pending_wait(bool &resumed, bool &observed_cancel) {
    CancellationSource source;

    // with_token() already returns a Cancellation-channel Task, so the
    // aggregate exposes Cancellation rather than propagating it into this frame.
    auto result = co_await WhenAll(with_token(cancelled_wait(resumed), source.token()), cancel_after(source, 5ms));

    observed_cancel = result.is_cancelled();
}

/// yield() resumes on a later loop iteration, strictly after callbacks that
/// were already queued when it suspended.
Task<void, Error> yields_after_pending_work(std::vector<std::string> &log) {
    auto timer = Timer::create();
    timer.start(0ms);

    co_await yield();
    log.push_back("yield");

    co_await timer.wait();
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

    loop.schedule(single_shot(single_log));
    loop.schedule(repeating(ticks));
    loop.schedule(sleeps_at_least(20ms, measured));
    loop.schedule(cancel_pending_wait(resumed, observed_cancel));
    loop.schedule(yields_after_pending_work(yield_log));
    loop.run();

    require(single_log.size() == 1 && single_log[0] == "fired", "single-shot Timer must fire exactly once");
    require(ticks == 5, "repeating Timer must fire 5 times, got " + std::to_string(ticks));
    // Allow the small early-fire slack described on sleeps_at_least().
    require(measured >= 15ms, "sleep(20ms) returned after only " + std::to_string(measured.count()) + "ms");
    require(!resumed, "a cancelled sleep must not resume its coroutine body");
    require(observed_cancel, "cancelling a pending Timer wait must surface as Cancellation");
    require(yield_log.size() == 2, "yield test must record both steps");

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
