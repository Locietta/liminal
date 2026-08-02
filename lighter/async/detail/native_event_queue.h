#pragma once

#include <deque>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>

namespace lighter::detail {

/// Loop-affine delivery queue for events produced by a native worker thread.
/// Producers post push() through Relay; consumers await next() on the loop.
template <typename T>
struct NativeEventQueue {
    std::deque<T> pending;
    Event ready;
    bool reading = false;

    void push(T event) {
        pending.push_back(std::move(event));
        ready.set();
    }

    Task<T, Error> next(Relay &relay, bool keep_loop_alive) {
        if (pending.empty()) {
            if (reading) {
                co_await fail(Error::k_connection_already_in_progress);
            }

            reading = true;
            if (keep_loop_alive) {
                relay.hold();
            }

            while (pending.empty()) {
                auto woken = co_await ready.wait().catch_cancel();
                if (woken.is_cancelled()) {
                    if (keep_loop_alive) {
                        relay.release();
                    }
                    reading = false;
                    co_await cancel();
                }
            }

            if (keep_loop_alive) {
                relay.release();
            }
            reading = false;
        }

        auto event = std::move(pending.front());
        pending.pop_front();
        if (pending.empty()) {
            ready.reset();
        }
        co_return event;
    }
};

} // namespace lighter::detail
