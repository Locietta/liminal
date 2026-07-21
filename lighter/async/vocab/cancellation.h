#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/runtime/when.h>

namespace lighter {

class CancellationToken {
public:
    class State {
    public:
        bool is_cancelled() const noexcept {
            return cancelled.load(std::memory_order_acquire);
        }

        void cancel() noexcept {
            bool expected = false;
            if (!cancelled.compare_exchange_strong(expected,
                                                   true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                return;
            }
            event.set();
        }

        /// Returns a Task that never succeeds.
        ///
        /// This awaitable turns the sticky Cancellation state stored in
        /// `cancel_event` back into Task Cancellation semantics.
        ///
        /// There are only two externally visible behaviors:
        /// 1. If the token is already cancelled, this Task cancels immediately.
        /// 2. Otherwise it waits until the token's internal Event is set, then
        ///    cancels itself immediately.
        ///
        /// The Event itself only wakes the waiter; it does not propagate
        /// Cancellation upward. The trailing `co_await cancel();` is therefore
        /// essential: it converts "the Cancellation Event has fired" into the
        /// coroutine state `CANCELLED`, which is what with_token(...) and other
        /// callers rely on.
        Task<> wait() {
            if (cancelled.load(std::memory_order_acquire)) {
                // Preserve Cancellation semantics for already-fired tokens.
                co_await lighter::cancel();
            }

            // Wait for the sticky Cancellation marker to become observable.
            co_await event.wait();

            // Being woken by cancel_event means "Cancellation happened"; convert
            // that wakeup into actual Task Cancellation.
            co_await lighter::cancel();
        }

    private:
        class Event event;
        std::atomic<bool> cancelled{false};
    };

    CancellationToken() noexcept = delete;
    CancellationToken(const CancellationToken &) noexcept = default;
    CancellationToken &operator=(const CancellationToken &) noexcept = default;

    bool cancelled() const noexcept {
        return state->is_cancelled();
    }

    Task<> wait() const noexcept {
        return state->wait();
    }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<State> state) : state(std::move(state)) {}

    std::shared_ptr<State> state;
};

class CancellationSource {
public:
    CancellationSource() : state(std::make_shared<class CancellationToken::State>()) {}

    CancellationSource(const CancellationSource &) = delete;
    CancellationSource &operator=(const CancellationSource &) = delete;

    ~CancellationSource() {
        cancel();
    }

    void cancel() noexcept {
        state->cancel();
    }

    bool cancelled() const noexcept {
        return state->is_cancelled();
    }

    CancellationToken token() const noexcept {
        return CancellationToken(state);
    }

private:
    std::shared_ptr<class CancellationToken::State> state;
};

/// with_token: cancel a Task when any of the given tokens fire.
/// Races the inner Task against token wait tasks using WhenAny;
/// if any token fires, WhenAny reports Cancellation explicitly.
template <typename T, typename E, typename C, std::same_as<CancellationToken>... Tokens>
    requires(sizeof...(Tokens) > 0)
Task<T, E, Cancellation> with_token(Task<T, E, C> inner_task, Tokens... tokens) {
    if ((tokens.cancelled() || ...)) {
        co_await cancel();
    }

    // Race the wrapped Task against all token waits.
    //
    // The token side is pure Cancellation: `tokens.wait()` never yields a value, it only
    // suspends until Cancellation interrupts the underlying Event wait. Because the inner
    // Task is wrapped with `catch_cancel()`, the aggregate also exposes Cancellation
    // explicitly, so a token firing now shows up as `race_result.is_cancelled()`.
    //
    // The inner Task is wrapped with `catch_cancel()`, so its Cancellation is converted into
    // the aggregate Cancellation channel instead of nesting inside the success payload.
    auto race_result = co_await WhenAny(std::move(inner_task).catch_cancel(), tokens.wait()...);

    if constexpr (!std::is_void_v<E>) {
        if (race_result.has_error()) {
            co_await fail(std::move(race_result).error());
        }
    }

    // guard value access with has_value() rather than relying on
    // co_await cancel() making subsequent code unreachable - MSVC's
    // coroutine codegen can fall through past a symmetric-transfer
    // suspension, reaching the dereference on a cancelled Outcome.
    if constexpr (!std::is_void_v<T>) {
        if (race_result.has_value()) {
            co_return std::move(std::get<0>(*race_result));
        }
    } else {
        if (race_result.has_value()) {
            co_return;
        }
    }

    co_await cancel();
}

} // namespace lighter
