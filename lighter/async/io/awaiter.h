#pragma once

#include <contracts>
#include <concepts>
#include <deque>
#include <optional>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/detail/libuv_helper.h>
#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/async/vocab/ringbuffer.h>

namespace lighter::uv {

template <typename StatusT>
inline bool is_cancelled_status(StatusT status) noexcept {
    return static_cast<i64>(status) == static_cast<i64>(UV_ECANCELED);
}

struct SingleWaiter {
    IoOp *waiter = nullptr;

    bool has_waiter() const noexcept { return waiter != nullptr; }

    void arm(IoOp &op) noexcept { waiter = &op; }

    void disarm() noexcept { waiter = nullptr; }

    template <typename StatusT>
    bool mark_cancelled_if(StatusT status) noexcept {
        if (waiter == nullptr || !is_cancelled_status(status)) {
            return false;
        }

        waiter->state = AsyncNode::CANCELLED;
        return true;
    }
};

template <typename ResultT>
struct WaiterBinding : SingleWaiter {
    ResultT *active = nullptr;

    void arm(IoOp &op, ResultT &slot) noexcept {
        this->waiter = &op;
        active = &slot;
    }

    void disarm() noexcept {
        SingleWaiter::disarm();
        active = nullptr;
    }

    bool try_deliver(ResultT &&value) {
        if (this->waiter == nullptr || active == nullptr) {
            return false;
        }

        *active = std::move(value);
        auto *w = this->waiter;
        disarm();
        w->complete();
        return true;
    }
};

template <typename Derived, AsyncNode::NodeKind Kind = AsyncNode::NodeKind::SYSTEM_IO>
struct AwaitOp : IoOp {
    AwaitOp() : IoOp(Kind) { this->action = &Derived::on_cancel; }

    template <typename CleanupFn>
    static void complete_cancel(IoOp *op, CleanupFn &&cleanup) noexcept pre(op != nullptr) {
        auto *aw = static_cast<Derived *>(op);
        cleanup(*aw);
        aw->complete();
    }

    template <typename StatusT>
    bool mark_cancelled_if(StatusT status) noexcept {
        if (!is_cancelled_status(status)) {
            return false;
        }

        this->state = AsyncNode::CANCELLED;
        return true;
    }
};

template <typename ResultT>
struct QueuedDelivery : WaiterBinding<ResultT> {
    std::deque<ResultT> pending;

    bool has_pending() const noexcept { return !pending.empty(); }

    ResultT take_pending() pre(!pending.empty()) {
        auto out = std::move(pending.front());
        pending.pop_front();
        return out;
    }

    void deliver(ResultT &&value) {
        if (!this->try_deliver(std::move(value))) {
            pending.push_back(std::move(value));
        }
    }

    void deliver(Error err)
        requires(!std::same_as<ResultT, Error>)
    {
        deliver(ResultT(outcome_error(err)));
    }
};

template <typename ResultT>
struct StoredDelivery : WaiterBinding<ResultT> {
    std::optional<ResultT> pending;

    bool has_pending() const noexcept { return pending.has_value(); }

    ResultT take_pending() pre(pending.has_value()) {
        auto out = std::move(*pending);
        pending.reset();
        return out;
    }

    void deliver(ResultT &&value) {
        if (!this->try_deliver(std::move(value))) {
            pending = std::move(value);
        }
    }

    void deliver(Error err)
        requires(!std::same_as<ResultT, Error>)
    {
        deliver(ResultT(outcome_error(err)));
    }
};

template <typename ResultT>
struct LatchedDelivery : WaiterBinding<ResultT> {
    std::optional<ResultT> pending;

    bool has_pending() const noexcept { return pending.has_value(); }

    const ResultT &peek_pending() const noexcept pre(pending.has_value()) { return *pending; }

    void deliver(ResultT value) {
        pending = value;
        this->try_deliver(std::move(value));
    }

    void deliver(Error err)
        requires(!std::same_as<ResultT, Error>)
    {
        deliver(ResultT(outcome_error(err)));
    }
};

template <typename ValueT>
struct LatestValueDelivery : WaiterBinding<Result<ValueT>> {
    std::optional<Result<ValueT>> pending;

    bool has_pending() const noexcept { return pending.has_value(); }

    Result<ValueT> take_pending() pre(pending.has_value()) {
        Result<ValueT> out(std::move(*pending));
        pending.reset();
        return out;
    }

    void deliver(Result<ValueT> &&value) {
        if (this->waiter != nullptr && this->active != nullptr) {
            this->try_deliver(std::move(value));
            return;
        }

        pending = std::move(value);
    }

    void deliver(Error err)
        requires(!std::same_as<ValueT, void>)
    {
        deliver(Result<ValueT>(outcome_error(err)));
    }
};

} // namespace lighter::uv

namespace lighter {

struct StreamHandle {
    union {
        uv_handle_t handle;
        uv_stream_t stream;
        uv_pipe_t pipe;
        uv_tcp_t tcp;
        uv_tty_t tty;
    };
};

struct Stream::Self : uv::handle<Stream::Self, uv_stream_t>, StreamHandle {
    enum class ReadMode { NONE, BUFFERED, DIRECT };

    uv::SingleWaiter reader;
    uv::SingleWaiter writer;
    RingBuffer buffer{};
    Error error_code{};
    ReadMode active_read_mode = ReadMode::NONE;
};

template <typename Stream>
struct Acceptor<Stream>::Self : uv::handle<Acceptor<Stream>::Self, uv_stream_t>, StreamHandle, uv::QueuedDelivery<Result<Stream>> {
    i32 pipe_ipc = 0;
};

} // namespace lighter
