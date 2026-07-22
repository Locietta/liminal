#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include <lighter/utils/function_traits.h>
#include <lighter/utils/functional.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

/// Run work on libuv's worker pool and complete when finished or with an Error.
Task<void, Error> queue(Function<void()> fn, EventLoop &loop = EventLoop::current());

/// Run work on libuv's worker pool, with a hook for chaining Cancellation.
///
/// If the awaiting Task is cancelled while the work is still queued, the work
/// is dequeued, `fn` never runs, and the hook is not invoked. If `fn` is
/// already running on a pool thread it cannot be interrupted; `on_cancel` is
/// how it learns it should return early - the Task settles as cancelled once
/// `fn` returns.
///
/// `on_cancel` runs on the loop thread, at most once, and can still fire
/// after `fn` has already finished. Keep it cheap and idempotent, and only
/// touch state that is safe to share with the concurrently running `fn` -
/// the typical shape is setting an atomic flag that `fn` polls.
Task<void, Error> queue(Function<void()> fn, Function<void()> on_cancel, EventLoop &loop = EventLoop::current());

/// Run work on libuv's worker pool and return either its value or an Error.
template <typename Fn, typename R = callable_return_t<Fn>>
    requires std::is_invocable_v<Fn> && (!std::is_void_v<R>)
Task<R, Error> queue(Fn fn, EventLoop &loop = EventLoop::current()) {
    std::optional<R> ret;
    co_await queue(Function<void()>([&] { ret.emplace(fn()); }), loop).or_fail();
    co_return std::move(*ret);
}

/// Value-returning variant of the Cancellation-hook overload.
template <typename Fn, typename R = callable_return_t<Fn>>
    requires std::is_invocable_v<Fn> && (!std::is_void_v<R>)
Task<R, Error> queue(Fn fn, Function<void()> on_cancel, EventLoop &loop = EventLoop::current()) {
    std::optional<R> ret;
    co_await queue(Function<void()>([&] { ret.emplace(fn()); }), std::move(on_cancel), loop).or_fail();
    co_return std::move(*ret);
}

} // namespace lighter
