#include <chrono>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

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

void check_support_table() {
    require(control_event_supported(ControlEventKind::INTERRUPT), "Ctrl+C must be supported");
#ifdef _WIN32
    require(!control_event_supported(ControlEventKind::TERMINATE), "Windows has no SIGTERM control");
    require(!control_event_supported(ControlEventKind::QUIT), "Windows has no SIGQUIT control");
    require(control_event_supported(ControlEventKind::BREAK), "Windows must support Ctrl+Break");
    require(!control_event_supported(ControlEventKind::LOGOFF), "interactive Windows apps do not receive logoff controls");
    require(!control_event_supported(ControlEventKind::SHUTDOWN), "interactive Windows apps do not receive shutdown controls");
    require(!control_event_supported(ControlEventKind::SUSPEND), "Windows has no POSIX terminal suspension control");
#else
    require(control_event_supported(ControlEventKind::TERMINATE), "POSIX must support SIGTERM");
    require(control_event_supported(ControlEventKind::QUIT), "POSIX must support SIGQUIT");
    require(!control_event_supported(ControlEventKind::BREAK), "Ctrl+Break is Windows-only");
    require(!control_event_supported(ControlEventKind::LOGOFF), "logoff is Windows-only");
    require(!control_event_supported(ControlEventKind::SHUTDOWN), "shutdown is Windows-only");
    require(control_event_supported(ControlEventKind::SUSPEND), "POSIX must support terminal suspension control");
#endif
    require(enum_name(ControlEventKind::INTERRUPT) == "INTERRUPT", "enum_name must render control events");
}

void check_invalid_sets() {
    EventLoop loop;
    require(!ControlEventSource::create({}, loop), "an empty control set must be rejected");
    require(!ControlEventSource::create({ControlEventKind::INTERRUPT, ControlEventKind::INTERRUPT}, loop),
            "duplicate controls must be rejected");

#ifdef _WIN32
    require(!ControlEventSource::create({ControlEventKind::TERMINATE}, loop), "unsupported Windows controls must be rejected");
#else
    require(!ControlEventSource::create({ControlEventKind::BREAK}, loop), "unsupported POSIX controls must be rejected");
#endif
}

void check_loop_holds_nest() {
    EventLoop loop;
    Relay inert;
    inert.release();

    auto source = ControlEventSource::create({ControlEventKind::INTERRUPT}, loop);
    require(source.has_value(), "ControlEventSource::create failed");
    require(!source->holding_loop(), "a fresh control source must not hold the loop");

    source->hold_loop();
    source->hold_loop();
    source->release_loop();
    require(source->holding_loop(), "an outer hold must survive an inner release");
    source->release_loop();
    require(!source->holding_loop(), "the final release must drop the loop hold");
}

void check_source_does_not_block_shutdown() {
    EventLoop loop;
    auto interrupts = InterruptSource::create(loop);
    require(interrupts.has_value(), "InterruptSource::create failed");

    bool ran = false;
    auto work = [](bool &ran) -> Task<void, Error> {
        co_await sleep(5ms);
        ran = true;
    };
    loop.schedule(work(ran));
    loop.run();
    require(ran, "scheduled work must run");
}

void check_source_can_die_before_run() {
    EventLoop loop;
    {
        auto interrupts = InterruptSource::create(loop);
        require(interrupts.has_value(), "InterruptSource::create failed");
    }

    bool ran = false;
    auto work = [](bool &ran) -> Task<void, Error> {
        ran = true;
        co_return;
    };
    loop.schedule(work(ran));
    loop.run();
    require(ran, "the loop must remain usable after source destruction");
}

#ifndef _WIN32

template <typename Fn>
void run_isolated(Fn &&make_task) {
    EventLoop loop;
    loop.schedule(make_task());
    loop.run();
}

Task<void, Error> reports_control(ControlEventKind &received) {
    auto source = ControlEventSource::create({ControlEventKind::INTERRUPT, ControlEventKind::TERMINATE}, EventLoop::current());
    require(source.has_value(), "ControlEventSource::create failed");

    auto raiser = []() -> Task<void, Error> {
        co_await sleep(5ms);
        std::raise(SIGTERM);
    };
    auto result = co_await WhenAll(source->next(), raiser());
    received = std::get<0>(*result);
}

Task<void, Error> queues_controls(std::vector<ControlEventKind> &received) {
    auto source = ControlEventSource::create({ControlEventKind::INTERRUPT, ControlEventKind::TERMINATE}, EventLoop::current());
    require(source.has_value(), "ControlEventSource::create failed");

    std::raise(SIGINT);
    std::raise(SIGTERM);
    co_await sleep(10ms);
    received.push_back(co_await source->next().or_fail());
    received.push_back(co_await source->next().or_fail());
}

Task<void, Error> interrupt_cancels_work(bool &cancelled, i32 &count) {
    auto interrupts = InterruptSource::create(EventLoop::current());
    require(interrupts.has_value(), "InterruptSource::create failed");

    auto workload = []() -> Task<void, Error> { co_await sleep(2s); };
    auto raiser = []() -> Task<void, Error> {
        co_await sleep(5ms);
        std::raise(SIGINT);
    };
    auto result = co_await WhenAll(with_token(workload(), interrupts->token()), raiser());
    cancelled = result.is_cancelled();
    count = interrupts->interrupt_count();
}

Task<void, Error> wait_keeps_loop_alive(bool &resumed) {
    auto source = ControlEventSource::create({ControlEventKind::INTERRUPT}, EventLoop::current());
    require(source.has_value(), "ControlEventSource::create failed");

    std::thread([] {
        std::this_thread::sleep_for(20ms);
        ::kill(::getpid(), SIGINT);
    }).detach();

    auto event = co_await source->next().catch_cancel();
    resumed = event.has_value() && *event == ControlEventKind::INTERRUPT;
}

#endif

i32 run_all() {
    check_support_table();
    check_invalid_sets();
    check_loop_holds_nest();
    check_source_does_not_block_shutdown();
    check_source_can_die_before_run();

#ifndef _WIN32
    auto received = ControlEventKind::QUIT;
    std::vector<ControlEventKind> queued;
    bool cancelled = false;
    i32 interrupt_count = 0;
    bool kept_alive = false;

    run_isolated([&] { return reports_control(received); });
    run_isolated([&] { return queues_controls(queued); });
    run_isolated([&] { return interrupt_cancels_work(cancelled, interrupt_count); });
    run_isolated([&] { return wait_keeps_loop_alive(kept_alive); });

    require(received == ControlEventKind::TERMINATE, "SIGTERM must map to TERMINATE");
    require(queued == std::vector{ControlEventKind::INTERRUPT, ControlEventKind::TERMINATE}, "controls must retain delivery order");
    require(cancelled, "a fatal control must cancel guarded work");
    require(interrupt_count == 1, "one fatal control must increment once");
    require(kept_alive, "a pending control wait must keep the loop alive");
#endif
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
