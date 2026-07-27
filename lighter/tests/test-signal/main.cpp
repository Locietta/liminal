#include <chrono>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/types.hpp>
#include <lighter/utils/enum.h>

namespace {

using namespace lighter;
using namespace std::chrono_literals;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

/// The kind/number mapping and the platform-support table must agree with each
/// other, and must be honest about the current platform. These run everywhere -
/// the delivery tests below cannot.
void check_signal_table() {
    require(signal_number(SignalKind::INT) == SIGINT, "INT must map to SIGINT");
    require(signal_number(SignalKind::TERM) == SIGTERM, "TERM must map to SIGTERM");
    require(signal_supported(SignalKind::INT), "Ctrl+C must be supported on every platform");

    // A kind the platform lacks must report unsupported rather than a bogus
    // number that would install a watcher which can never fire.
    for (auto kind : {SignalKind::INT, SignalKind::TERM, SignalKind::HUP, SignalKind::QUIT, SignalKind::BREAK, SignalKind::WINCH}) {
        if (signal_number(kind) < 0) {
            require(!signal_supported(kind), std::string(enum_name(kind)) + " has no number but claims support");
        }
    }

#ifdef _WIN32
    require(!signal_supported(SignalKind::TERM), "Windows cannot deliver SIGTERM");
    require(!signal_supported(SignalKind::WINCH), "Windows cannot deliver SIGWINCH");
    require(signal_supported(SignalKind::BREAK), "Ctrl+Break must be supported on Windows");
#else
    require(signal_supported(SignalKind::TERM), "POSIX must deliver SIGTERM");
    require(signal_supported(SignalKind::WINCH), "POSIX must deliver SIGWINCH");
    require(!signal_supported(SignalKind::BREAK), "SIGBREAK is Windows-only");
#endif

    require(enum_name(SignalKind::WINCH) == "WINCH", "enum_name must render SignalKind");
}

/// An empty set is a caller mistake, not an object that silently never fires.
void check_empty_set_rejected() {
    EventLoop loop;
    auto empty = SignalSet::create({}, loop);
    require(!empty.has_value(), "an empty SignalSet must be rejected");
}

// Signal delivery cannot be exercised on Windows: libuv synthesizes its signals
// from console control events, and raise() explicitly does not trigger them, so
// the tests below would hang rather than fail. See SignalKind in watcher.h.
#ifndef _WIN32

/// A signal raised while a waiter is parked resolves that wait, and reports
/// which signal it was rather than merely that something happened.
Task<void, Error> reports_which_signal(SignalKind &got) {
    auto set = SignalSet::create({SignalKind::INT, SignalKind::TERM}, EventLoop::current());
    require(set.has_value(), "SignalSet::create failed");

    auto raiser = []() -> Task<void, Error> {
        co_await sleep(5ms);
        std::raise(SIGTERM);
    };

    auto result = co_await WhenAll(set->wait(), raiser());
    got = std::get<0>(*result);
}

/// Signals that arrive with nobody waiting are queued, not dropped, and drain
/// in arrival order. A second Ctrl+C during cleanup must stay visible.
Task<void, Error> queues_while_idle(std::vector<SignalKind> &drained) {
    auto set = SignalSet::create({SignalKind::INT, SignalKind::TERM}, EventLoop::current());
    require(set.has_value(), "SignalSet::create failed");

    // Raise both before ever waiting, so neither can be delivered to a waiter.
    std::raise(SIGINT);
    std::raise(SIGTERM);

    // Let the loop turn so libuv delivers both callbacks.
    co_await sleep(10ms);

    drained.push_back(co_await set->wait().or_fail());
    drained.push_back(co_await set->wait().or_fail());
}

/// The whole point of the facade: a signal cancels work through the ordinary
/// with_token() path, without the workload knowing signals exist.
Task<void, Error> interrupt_cancels_work(bool &cancelled, bool &finished, i32 &count) {
    auto interrupts = InterruptSource::create(EventLoop::current());
    require(interrupts.has_value(), "InterruptSource::create failed");

    auto workload = [](bool &finished) -> Task<void, Error> {
        co_await sleep(2s);
        finished = true;
    };

    auto raiser = []() -> Task<void, Error> {
        co_await sleep(5ms);
        std::raise(SIGINT);
    };

    auto guarded = with_token(workload(finished), interrupts->token());
    auto result = co_await WhenAll(std::move(guarded), raiser());

    cancelled = result.is_cancelled();
    count = interrupts->interrupt_count();
}

/// Non-fatal signals stay observable through next() without cancelling. This is
/// the SIGWINCH case: a resize must not tear the program down.
Task<void, Error> observes_without_cancelling(SignalKind &seen, bool &still_live) {
    auto interrupts = InterruptSource::create({SignalKind::TERM}, {SignalKind::WINCH}, EventLoop::current());
    require(interrupts.has_value(), "InterruptSource::create failed");

    auto raiser = []() -> Task<void, Error> {
        co_await sleep(5ms);
        std::raise(SIGWINCH);
    };

    auto result = co_await WhenAll(interrupts->next(), raiser());
    seen = std::get<0>(*result);
    still_live = !interrupts->interrupted();
}

#endif // !_WIN32

/// Runs one Task to completion on its own loop.
///
/// Each signal case gets a private loop and runs to completion before the next
/// starts. Signals are process-wide: two SignalSets alive at once both receive
/// every raise(), so running these concurrently would let one case consume
/// another's signal. That is a property of POSIX, not of SignalSet.
template <typename Fn>
void run_isolated(Fn &&make_task) {
    EventLoop loop;
    loop.schedule(make_task());
    loop.run();
}

i32 run_all() {
    check_signal_table();
    check_empty_set_rejected();

#ifndef _WIN32
    auto got = SignalKind::QUIT;
    std::vector<SignalKind> drained;
    bool cancelled = false;
    bool finished = false;
    i32 count = 0;
    auto seen = SignalKind::INT;
    bool still_live = false;

    run_isolated([&] { return reports_which_signal(got); });
    run_isolated([&] { return queues_while_idle(drained); });
    run_isolated([&] { return interrupt_cancels_work(cancelled, finished, count); });
    run_isolated([&] { return observes_without_cancelling(seen, still_live); });

    require(got == SignalKind::TERM, "SignalSet must report TERM, got " + std::string(enum_name(got)));
    require(drained.size() == 2, "both queued signals must be drained");
    require(drained[0] == SignalKind::INT && drained[1] == SignalKind::TERM, "queued signals must drain in arrival order");
    require(cancelled, "a fatal signal must cancel work through its token");
    require(!finished, "cancelled work must not run to completion");
    require(count == 1, "one fatal signal must count once, got " + std::to_string(count));
    require(seen == SignalKind::WINCH, "next() must surface a non-fatal signal");
    require(still_live, "a non-fatal signal must not cancel the token");
#endif

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
