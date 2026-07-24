#include "stream.h"

#include <cassert>
#include <type_traits>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>

namespace lighter {

namespace {

template <typename T>
constexpr inline bool always_false_v = false;

Result<u32> to_uv_pipe_flags(const Pipe::Options &opts) {
    u32 out = 0;
#ifdef UV_PIPE_NO_TRUNCATE
    if (opts.no_truncate) {
        out |= UV_PIPE_NO_TRUNCATE;
    }
#else
    if (opts.no_truncate) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
    return out;
}

Result<u32> to_uv_pipe_connect_flags(const Pipe::Options &opts) { return to_uv_pipe_flags(opts); }

Result<u32> to_uv_tcp_bind_flags(const Tcp::Options &opts) {
    u32 out = 0;
#ifdef UV_TCP_IPV6ONLY
    if (opts.ipv6_only) {
        out |= UV_TCP_IPV6ONLY;
    }
#else
    if (opts.ipv6_only) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_TCP_REUSEPORT
    if (opts.reuse_port) {
        out |= UV_TCP_REUSEPORT;
    }
#else
    if (opts.reuse_port) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
    return out;
}

template <typename Stream>
struct AcceptAwait : uv::AwaitOp<AcceptAwait<Stream>> {
    using await_base = uv::AwaitOp<AcceptAwait<Stream>>;
    using promise_t = Task<Stream, Error>::promise_type;
    using self_t = typename Acceptor<Stream>::Self;

    // Acceptor self used for waiter registration and pending queueing.
    self_t *self;
    // Result slot populated by connection callbacks.
    Result<Stream> outcome = outcome_error(Error());

    explicit AcceptAwait(self_t *acceptor) : self(acceptor) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                aw.self->disarm();
            }
        });
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }
        self->arm(*this, outcome);
        return this->attach(waiting.promise(), loc);
    }

    Result<Stream> await_resume() noexcept {
        if (self) {
            self->disarm();
        }
        return std::move(outcome);
    }
};

template <typename Stream>
void on_connection(uv_stream_t *server, i32 status) {
    using self_t = typename Acceptor<Stream>::Self;

    assert(server != nullptr && "on_connection requires non-null server");
    auto *listener = static_cast<self_t *>(server->data);
    assert(listener != nullptr && "on_connection requires listener state in server->data");

    if (auto err = uv::status_to_error(status)) {
        listener->deliver(err);
        return;
    }

    auto self = Stream::Self::make();
    Error err{};
    if constexpr (std::is_same_v<Stream, Pipe>) {
        err = uv::pipe_init(*server->loop, self->pipe, listener->pipe_ipc);
    } else if constexpr (std::is_same_v<Stream, Tcp>) {
        err = uv::tcp_init(*server->loop, self->tcp);
    } else {
        static_assert(always_false_v<Stream>, "unsupported accept Stream type");
    }

    if (!err) {
        err = uv::accept(*server, self->stream);
    }

    if (err) {
        listener->deliver(err);
    } else {
        listener->deliver(Stream(std::move(self)));
    }
}

template <typename Stream>
struct ConnectAwait : uv::AwaitOp<ConnectAwait<Stream>> {
    using await_base = uv::AwaitOp<ConnectAwait<Stream>>;
    using promise_t = Task<Stream, Error>::promise_type;
    using self_ptr = Stream::Self::pointer;

    // Candidate Stream self; reset on cancel to close the handle.
    self_ptr self;
    // libuv connect request; req.data points back to this Awaiter.
    uv_connect_t req{};
    // Pipe name kept alive for uv_pipe_connect2().
    std::string name;
    // Pipe connect flags.
    u32 flags = 0;
    // Resolved peer address for uv_tcp_connect().
    sockaddr_storage addr{};
    // Result slot returned from await_resume().
    Result<Stream> outcome = outcome_error(Error());
    // Constructor-level validation flag.
    bool ready = true;

    ConnectAwait(self_ptr self, std::string_view name, Pipe::Options opts) : self(std::move(self)), name(name) {
        if constexpr (std::is_same_v<Stream, Pipe>) {
            if (this->name.empty()) {
                ready = false;
                outcome = outcome_error(Error::k_invalid_argument);
                return;
            }

            auto uv_flags = to_uv_pipe_connect_flags(opts);
            if (!uv_flags) {
                ready = false;
                outcome = outcome_error(uv_flags.error());
                return;
            }
            flags = uv_flags.value();
        } else {
            static_assert(always_false_v<Stream>, "Pipe constructor requires Stream=Pipe");
        }
    }

    ConnectAwait(self_ptr self, std::string_view host, i32 port) : self(std::move(self)) {
        if constexpr (std::is_same_v<Stream, Tcp>) {
            auto resolved = uv::resolve_addr(host, port);
            if (!resolved) {
                ready = false;
                outcome = outcome_error(resolved.error());
                return;
            }
            addr = resolved->storage;
        } else {
            static_assert(always_false_v<Stream>, "Tcp constructor requires Stream=Tcp");
        }
    }

    static void on_cancel(IoOp *op) {
        auto *aw = static_cast<ConnectAwait *>(op);
        if (aw->self) {
            // uv_connect_t can't be cancelled; close handle to trigger UV_ECANCELED callback.
            aw->self.reset();
        }
    }

    static void on_connect(uv_connect_t *req, i32 status) {
        auto *aw = static_cast<ConnectAwait *>(req->data);
        assert(aw != nullptr && "on_connect requires Awaiter in req->data");

        aw->mark_cancelled_if(status);

        if (auto err = uv::status_to_error(status)) {
            aw->outcome = outcome_error(err);
        } else if (aw->self) {
            if constexpr (std::is_same_v<Stream, Pipe>) {
                aw->outcome = Pipe(std::move(aw->self));
            } else if constexpr (std::is_same_v<Stream, Tcp>) {
                aw->outcome = Tcp(std::move(aw->self));
            } else {
                static_assert(always_false_v<Stream>, "unsupported connect Stream type");
            }
        } else {
            aw->outcome = outcome_error(Error::k_invalid_argument);
        }

        aw->complete();
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self || !ready) {
            return waiting;
        }

        req.data = this;

        Error err{};
        if constexpr (std::is_same_v<Stream, Pipe>) {
            err = uv::pipe_connect2(req, self->pipe, name.c_str(), name.size(), flags, on_connect);
        } else if constexpr (std::is_same_v<Stream, Tcp>) {
            err = uv::tcp_connect(req, self->tcp, reinterpret_cast<const sockaddr *>(&addr), on_connect);
        } else {
            static_assert(always_false_v<Stream>, "unsupported connect Stream type");
        }

        if (err) {
            outcome = outcome_error(err);
            return waiting;
        }

        return this->attach(waiting.promise(), loc);
    }

    Result<Stream> await_resume() noexcept { return std::move(outcome); }
};

} // namespace

template <typename Stream>
Acceptor<Stream>::Acceptor() noexcept = default;

template <typename Stream>
Acceptor<Stream>::Acceptor(Acceptor &&other) noexcept = default;

template <typename Stream>
Acceptor<Stream> &Acceptor<Stream>::operator=(Acceptor &&other) noexcept = default;

template <typename Stream>
Acceptor<Stream>::~Acceptor() = default;

template <typename Stream>
typename Acceptor<Stream>::Self *Acceptor<Stream>::operator->() noexcept {
    return self.get();
}

template <typename Stream>
Task<Stream, Error> Acceptor<Stream>::accept() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->has_pending()) {
        co_return self->take_pending();
    }

    if (self->has_waiter()) {
        co_await fail(Error::k_connection_already_in_progress);
    }

    co_return co_await AcceptAwait<Stream>{self.get()};
}

template <typename Stream>
Error Acceptor<Stream>::stop() {
    if (!self) {
        return Error::k_invalid_argument;
    }

    self->deliver(Error::k_operation_aborted);

    return {};
}

template <typename Stream>
Acceptor<Stream>::Acceptor(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

template struct Acceptor<Pipe>;
template struct Acceptor<Tcp>;

Result<Pipe> Pipe::open(i32 fd, Pipe::Options opts, EventLoop &loop) {
    auto pipe_res = create(opts, loop);
    if (!pipe_res) {
        return outcome_error(pipe_res.error());
    }

    auto &handle = pipe_res->self->pipe;
    if (auto err = uv::pipe_open(handle, fd)) {
        return outcome_error(err);
    }

    return std::move(*pipe_res);
}

Result<Pipe::Acceptor> Pipe::listen(std::string_view name, Pipe::Options opts, EventLoop &loop) {
    auto self = Pipe::Acceptor::Self::make();
    if (auto err = uv::pipe_init(loop, self->pipe, opts.ipc ? 1 : 0)) {
        return outcome_error(err);
    }

    auto &acc = *self;
    acc.pipe_ipc = opts.ipc ? 1 : 0;
    auto &handle = acc.pipe;

    auto uv_flags = to_uv_pipe_flags(opts);
    if (!uv_flags) {
        return outcome_error(uv_flags.error());
    }

    if (name.empty()) {
        return outcome_error(Error::k_invalid_argument);
    }

    if (auto err = uv::pipe_bind2(handle, name.data(), name.size(), uv_flags.value())) {
        return outcome_error(err);
    }

    if (auto err = uv::listen(handle, opts.backlog, on_connection<Pipe>)) {
        return outcome_error(err);
    }

    return Pipe::Acceptor(std::move(self));
}

Pipe::Pipe(UniqueHandle<Self> self) noexcept : Stream(std::move(self)) {}

Result<Pipe> Pipe::create(Pipe::Options opts, EventLoop &loop) {
    auto self = Self::make();
    if (auto err = uv::pipe_init(loop, self->pipe, opts.ipc ? 1 : 0)) {
        return outcome_error(err);
    }

    return Pipe(std::move(self));
}

Task<Pipe, Error> Pipe::connect(std::string_view name, Pipe::Options opts, EventLoop &loop) {
    auto self = Self::make();
    if (auto err = uv::pipe_init(loop, self->pipe, opts.ipc ? 1 : 0)) {
        co_await fail(err);
    }

    co_return co_await ConnectAwait<Pipe>{std::move(self), name, opts};
}

Tcp::Tcp(UniqueHandle<Self> self) noexcept : Stream(std::move(self)) {}

Result<Tcp> Tcp::open(i32 fd, EventLoop &loop) {
    auto self = Self::make();
    if (auto err = uv::tcp_init(loop, self->tcp)) {
        return outcome_error(err);
    }

    if (auto err = uv::tcp_open(self->tcp, fd)) {
        return outcome_error(err);
    }

    return Tcp(std::move(self));
}

Task<Tcp, Error> Tcp::connect(std::string_view host, i32 port, EventLoop &loop) {
    auto self = Self::make();
    if (auto err = uv::tcp_init(loop, self->tcp)) {
        co_await fail(err);
    }

    co_return co_await ConnectAwait<Tcp>{std::move(self), host, port};
}

Result<Tcp::Acceptor> Tcp::listen(std::string_view host, i32 port, Tcp::Options opts, EventLoop &loop) {
    auto self = Tcp::Acceptor::Self::make();
    if (auto err = uv::tcp_init(loop, self->tcp)) {
        return outcome_error(err);
    }

    auto &acc = *self;
    auto &handle = acc.tcp;

    auto resolved = uv::resolve_addr(host, port);
    if (!resolved) {
        return outcome_error(resolved.error());
    }

    ::sockaddr *addr_ptr = reinterpret_cast<sockaddr *>(&resolved->storage);

    auto uv_flags = to_uv_tcp_bind_flags(opts);
    if (!uv_flags) {
        return outcome_error(uv_flags.error());
    }

    if (auto err = uv::tcp_bind(handle, addr_ptr, uv_flags.value())) {
        return outcome_error(err);
    }

    if (auto err = uv::listen(handle, opts.backlog, on_connection<Tcp>)) {
        return outcome_error(err);
    }

    return Tcp::Acceptor(std::move(self));
}

Result<i32> Tcp::local_port(Tcp::Acceptor &acc) {
    if (!acc.self) {
        return outcome_error(Error::k_invalid_argument);
    }

    sockaddr_storage storage{};
    i32 namelen = sizeof(storage);
    i32 err = uv_tcp_getsockname(&acc->tcp, reinterpret_cast<sockaddr *>(&storage), &namelen);
    if (err != 0) {
        return outcome_error(uv::status_to_error(err));
    }

    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in *>(&storage)->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6 *>(&storage)->sin6_port);
    }

    return outcome_error(Error::k_invalid_argument);
}

} // namespace lighter
