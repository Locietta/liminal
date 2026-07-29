#include "udp.h"

#include <contracts>
#include <optional>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

static Result<Udp::Endpoint> endpoint_from_sockaddr(const sockaddr *addr);

struct Udp::Self : uv::handle<Udp::Self, uv_udp_t> {
    uv_udp_t handle{};
    uv::QueuedDelivery<Result<Udp::RecvResult>> recv;
    std::vector<char> buffer;
    bool receiving = false;

    uv::StoredDelivery<Error> send;
    bool send_inflight = false;
};

namespace {

constexpr usize k_udp_recv_buffer_size = 64 * 1024;

static Udp::Self::pointer make_udp_self() {
    auto self = Udp::Self::make();
    self->buffer.resize(k_udp_recv_buffer_size);
    return self;
}

static Result<u32> to_uv_udp_init_flags(const Udp::CreateOptions &options) {
    u32 out = 0;
#ifdef UV_UDP_IPV6ONLY
    if (options.ipv6_only) {
        out |= UV_UDP_IPV6ONLY;
    }
#else
    if (options.ipv6_only) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_UDP_RECVMMSG
    if (options.recvmmsg) {
        out |= UV_UDP_RECVMMSG;
    }
#else
    if (options.recvmmsg) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
    return out;
}

static Result<u32> to_uv_udp_bind_flags(const Udp::BindOptions &options) {
    u32 out = 0;
#ifdef UV_UDP_IPV6ONLY
    if (options.ipv6_only) {
        out |= UV_UDP_IPV6ONLY;
    }
#else
    if (options.ipv6_only) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_UDP_REUSEADDR
    if (options.reuse_addr) {
        out |= UV_UDP_REUSEADDR;
    }
#else
    if (options.reuse_addr) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_UDP_REUSEPORT
    if (options.reuse_port) {
        out |= UV_UDP_REUSEPORT;
    }
#else
    if (options.reuse_port) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
    return out;
}

static Udp::RecvFlags to_udp_recv_flags([[maybe_unused]] u32 flags) {
    Udp::RecvFlags out{};
#ifdef UV_UDP_PARTIAL
    if ((flags & UV_UDP_PARTIAL) != 0) {
        out.partial = true;
    }
#endif
#ifdef UV_UDP_MMSG_CHUNK
    if ((flags & UV_UDP_MMSG_CHUNK) != 0) {
        out.mmsg_chunk = true;
    }
#endif
    return out;
}

struct UdpRecvAwait : uv::AwaitOp<UdpRecvAwait> {
    using await_base = uv::AwaitOp<UdpRecvAwait>;
    using promise_t = Task<Udp::RecvResult, Error>::promise_type;

    // UDP socket self used to register waiter and manage recv lifecycle.
    Udp::Self *self;
    // Result slot written by on_read() and returned from await_resume().
    Result<Udp::RecvResult> outcome = outcome_error(Error());

    explicit UdpRecvAwait(Udp::Self *socket) : self(socket) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self && aw.self->receiving) {
                uv::udp_recv_stop(aw.self->handle);
                aw.self->receiving = false;
            }
            if (aw.self) {
                aw.self->recv.disarm();
            }
        });
    }

    static void on_alloc(uv_handle_t *handle, usize, uv_buf_t *buf) {
        auto *u = static_cast<Udp::Self *>(handle->data);
        contract_assert(u != nullptr);

        buf->base = u->buffer.data();
        buf->len = static_cast<decltype(buf->len)>(u->buffer.size());
    }

    static void on_read(uv_udp_t *handle, isize nread, const uv_buf_t *buf, const struct sockaddr *addr, u32 flags) {
        auto *u = static_cast<Udp::Self *>(handle->data);
        contract_assert(u != nullptr);

        if (auto err = uv::status_to_error(nread)) {
            u->recv.mark_cancelled_if(nread);
            u->recv.deliver(err);
            return;
        }

        // libuv reports nread == 0 with no address when there is nothing to
        // deliver. For recvmmsg this is also the final UV_UDP_MMSG_FREE
        // notification for the original allocation.
        if (nread == 0 && addr == nullptr) {
            return;
        }

        Udp::RecvResult out{};
        if (nread > 0) {
            contract_assert(buf != nullptr && buf->base != nullptr);
            out.data.assign(buf->base, buf->base + static_cast<usize>(nread));
        }
        out.flags = to_udp_recv_flags(flags);

        if (addr != nullptr) {
            auto ep = endpoint_from_sockaddr(addr);
            if (ep) {
                out.addr = std::move(ep->addr);
                out.port = ep->port;
            }
        }

        u->recv.deliver(std::move(out));
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }

        self->recv.arm(*this, outcome);

        if (!self->receiving) {
            auto err = uv::udp_recv_start(self->handle, on_alloc, on_read);
            if (!err) {
                self->receiving = true;
            } else {
                outcome = outcome_error(err);
                self->recv.disarm();
                return waiting;
            }
        }

        return this->attach(waiting.promise(), loc);
    }

    Result<Udp::RecvResult> await_resume() noexcept {
        if (self) {
            self->recv.disarm();
        }
        return std::move(outcome);
    }
};

struct UdpSendAwait : uv::AwaitOp<UdpSendAwait> {
    using promise_t = Task<void, Error>::promise_type;

    // UDP socket self that owns send waiter and inflight flags.
    Udp::Self *self;
    // Owns outbound bytes until on_send() runs.
    std::vector<char> storage;
    // libuv send request; req.handle gives us the socket on completion.
    uv_udp_send_t req{};
    // Optional destination for unconnected sockets.
    std::optional<sockaddr_storage> dest;
    // Completion status returned from await_resume().
    Error result;

    UdpSendAwait(Udp::Self *u, std::span<const char> data, std::optional<sockaddr_storage> &&d)
        : self(u), storage(data.begin(), data.end()), dest(std::move(d)) {}

    static void on_cancel(IoOp *op) {
        auto *aw = static_cast<UdpSendAwait *>(op);
        if (!aw->self) {
            return;
        }
        // uv_udp_send_t is not cancellable via uv_cancel().
        // Keep the request in-flight and wait for on_send() to retire it.
    }

    static void on_send(uv_udp_send_t *req, i32 status) {
        auto *handle = static_cast<uv_udp_t *>(req->handle);
        contract_assert(handle != nullptr);
        auto *u = static_cast<Udp::Self *>(handle->data);
        contract_assert(u != nullptr);

        u->send_inflight = false;

        u->send.mark_cancelled_if(status);

        auto ec = uv::status_to_error(status);

        u->send.deliver(std::move(ec));
    }

    bool await_ready() noexcept {
        if (self && self->send.has_pending()) {
            result = self->send.take_pending();
            return true;
        }
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            result = Error::k_invalid_argument;
            return waiting;
        }

        if (self->send.has_waiter() || self->send_inflight) {
            result = Error::k_connection_already_in_progress;
            return waiting;
        }

        self->send.arm(*this, result);

        uv_buf_t buf = uv::buf_init(storage.empty() ? nullptr : storage.data(), static_cast<u32>(storage.size()));

        const sockaddr *addr = dest.has_value() ? reinterpret_cast<const sockaddr *>(&dest.value()) : nullptr;

        if (auto err = uv::udp_send(req, self->handle, std::span<const uv_buf_t>{&buf, 1}, addr, on_send)) {
            result = err;
            self->send.disarm();
            return waiting;
        }

        self->send_inflight = true;
        return this->attach(waiting.promise(), loc);
    }

    Error await_resume() noexcept {
        if (self) {
            self->send.disarm();
        }
        return result;
    }
};

} // namespace

Udp::Udp() noexcept = default;

Udp::Udp(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

Udp::~Udp() = default;

Udp::Udp(Udp &&other) noexcept = default;

Udp &Udp::operator=(Udp &&other) noexcept = default;

Udp::Self *Udp::operator->() noexcept { return self.get(); }

static Result<Udp::Endpoint> endpoint_from_sockaddr(const sockaddr *addr) {
    if (addr == nullptr) {
        return outcome_error(Error::k_invalid_argument);
    }

    Udp::Endpoint out{};
    char host[INET6_ADDRSTRLEN]{};
    i32 port = 0;
    if (addr->sa_family == AF_INET) {
        auto *in = reinterpret_cast<const sockaddr_in *>(addr);
        if (auto err = uv::ip4_name(*in, host, sizeof(host))) {
            return outcome_error(err);
        }
        port = ntohs(in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        if (auto err = uv::ip6_name(*in6, host, sizeof(host))) {
            return outcome_error(err);
        }
        port = ntohs(in6->sin6_port);
    } else {
        return outcome_error(Error::k_invalid_argument);
    }

    out.addr = host;
    out.port = port;
    return out;
}

Result<Udp> Udp::create(EventLoop &loop) {
    auto self = make_udp_self();
    if (auto err = uv::udp_init(loop, self->handle)) {
        return outcome_error(err);
    }

    return Udp(std::move(self));
}

Result<Udp> Udp::create(CreateOptions options, EventLoop &loop) {
    auto self = make_udp_self();
    auto uv_flags = to_uv_udp_init_flags(options);
    if (!uv_flags) {
        return outcome_error(uv_flags.error());
    }

    if (auto err = uv::udp_init_ex(loop, self->handle, uv_flags.value())) {
        return outcome_error(err);
    }

    return Udp(std::move(self));
}

Result<Udp> Udp::open(i32 fd, EventLoop &loop) {
    auto self = make_udp_self();
    if (auto err = uv::udp_init(loop, self->handle)) {
        return outcome_error(err);
    }

    if (auto err = uv::udp_open(self->handle, fd)) {
        return outcome_error(err);
    }

    return Udp(std::move(self));
}

Error Udp::bind(std::string_view host, i32 port, BindOptions options) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    auto uv_flags = to_uv_udp_bind_flags(options);
    if (!uv_flags) {
        return uv_flags.error();
    }

    auto resolved = uv::resolve_addr(host, port);
    if (!resolved) {
        return resolved.error();
    }

    const sockaddr *addr = reinterpret_cast<const sockaddr *>(&resolved->storage);
    if (auto err = uv::udp_bind(self->handle, addr, uv_flags.value())) {
        return err;
    }

    return {};
}

Error Udp::connect(std::string_view host, i32 port) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    auto resolved = uv::resolve_addr(host, port);
    if (!resolved) {
        return resolved.error();
    }

    const sockaddr *addr = reinterpret_cast<const sockaddr *>(&resolved->storage);
    if (auto err = uv::udp_connect(self->handle, addr)) {
        return err;
    }

    return {};
}

Error Udp::disconnect() {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::udp_connect(self->handle, nullptr)) {
        return err;
    }

    return {};
}

Task<void, Error> Udp::send(std::span<const char> data, std::string_view host, i32 port) {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    auto resolved = uv::resolve_addr(host, port);
    if (!resolved) {
        co_await fail(resolved.error());
    }

    if (auto err = co_await UdpSendAwait{self.get(), data, std::optional<sockaddr_storage>(resolved->storage)}) {
        co_await fail(std::move(err));
    }
}

Task<void, Error> Udp::send(std::span<const char> data) {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (auto err = co_await UdpSendAwait{self.get(), data, std::nullopt}) {
        co_await fail(std::move(err));
    }
}

Error Udp::try_send(std::span<const char> data, std::string_view host, i32 port) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    auto resolved = uv::resolve_addr(host, port);
    if (!resolved) {
        return resolved.error();
    }

    uv_buf_t buf = uv::buf_init(const_cast<char *>(data.data()), static_cast<u32>(data.size()));
    if (auto sent =
            uv::udp_try_send(self->handle, std::span<const uv_buf_t>{&buf, 1}, reinterpret_cast<const sockaddr *>(&resolved->storage));
        !sent) {
        return sent.error();
    }

    return {};
}

Error Udp::try_send(std::span<const char> data) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    uv_buf_t buf = uv::buf_init(const_cast<char *>(data.data()), static_cast<u32>(data.size()));
    if (auto sent = uv::udp_try_send(self->handle, std::span<const uv_buf_t>{&buf, 1}, nullptr); !sent) {
        return sent.error();
    }

    return {};
}

Error Udp::stop_recv() {
    if (!self) {
        return Error::k_invalid_argument;
    }

    uv::udp_recv_stop(self->handle);
    self->receiving = false;
    return {};
}

Task<Udp::RecvResult, Error> Udp::recv() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->recv.has_pending()) {
        co_return self->recv.take_pending();
    }

    if (self->recv.has_waiter()) {
        co_await fail(Error::k_connection_already_in_progress);
    }

    co_return co_await UdpRecvAwait{self.get()};
}

Result<Udp::Endpoint> Udp::getsockname() const {
    if (!self) {
        return outcome_error(Error::k_invalid_argument);
    }

    sockaddr_storage storage{};
    i32 len = sizeof(storage);
    if (auto err = uv::udp_getsockname(self->handle, *reinterpret_cast<sockaddr *>(&storage), len)) {
        return outcome_error(err);
    }

    return endpoint_from_sockaddr(reinterpret_cast<sockaddr *>(&storage));
}

Result<Udp::Endpoint> Udp::getpeername() const {
    if (!self) {
        return outcome_error(Error::k_invalid_argument);
    }

    sockaddr_storage storage{};
    i32 len = sizeof(storage);
    if (auto err = uv::udp_getpeername(self->handle, *reinterpret_cast<sockaddr *>(&storage), len)) {
        return outcome_error(err);
    }

    return endpoint_from_sockaddr(reinterpret_cast<sockaddr *>(&storage));
}

Error Udp::set_membership(std::string_view multicast_addr, std::string_view interface_addr, Membership m) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    std::string multicast_storage(multicast_addr);
    std::string interface_storage(interface_addr);
    if (auto err = uv::udp_set_membership(self->handle, multicast_storage.c_str(), interface_storage.c_str(),
                                          m == Membership::JOIN ? UV_JOIN_GROUP : UV_LEAVE_GROUP)) {
        return err;
    }

    return {};
}

Error Udp::set_source_membership(std::string_view multicast_addr, std::string_view interface_addr, std::string_view source_addr,
                                 Membership m) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    std::string multicast_storage(multicast_addr);
    std::string interface_storage(interface_addr);
    std::string source_storage(source_addr);
    if (auto err = uv::udp_set_source_membership(self->handle, multicast_storage.c_str(), interface_storage.c_str(), source_storage.c_str(),
                                                 m == Membership::JOIN ? UV_JOIN_GROUP : UV_LEAVE_GROUP)) {
        return err;
    }

    return {};
}

Error Udp::set_multicast_loop(bool on) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::udp_set_multicast_loop(self->handle, on)) {
        return err;
    }

    return {};
}

Error Udp::set_multicast_ttl(i32 ttl) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::udp_set_multicast_ttl(self->handle, ttl)) {
        return err;
    }

    return {};
}

Error Udp::set_multicast_interface(std::string_view interface_addr) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    std::string interface_storage(interface_addr);
    if (auto err = uv::udp_set_multicast_interface(self->handle, interface_storage.c_str())) {
        return err;
    }

    return {};
}

Error Udp::set_broadcast(bool on) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::udp_set_broadcast(self->handle, on)) {
        return err;
    }

    return {};
}

Error Udp::set_ttl(i32 ttl) {
    if (!self) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::udp_set_ttl(self->handle, ttl)) {
        return err;
    }

    return {};
}

bool Udp::using_recvmmsg() const {
    if (!self) {
        return false;
    }

    return uv::udp_using_recvmmsg(self->handle);
}

usize Udp::send_queue_size() const {
    if (!self) {
        return 0;
    }

    return uv::udp_get_send_queue_size(self->handle);
}

usize Udp::send_queue_count() const {
    if (!self) {
        return 0;
    }

    return uv::udp_get_send_queue_count(self->handle);
}

} // namespace lighter
