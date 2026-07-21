#pragma once

#include <span>
#include <string>
#include <string_view>

#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>
#include <lighter/types.hpp>

namespace lighter {

class EventLoop;

class Udp {
public:
    Udp() noexcept;

    Udp(const Udp &) = delete;
    Udp &operator=(const Udp &) = delete;

    Udp(Udp &&other) noexcept;

    Udp &operator=(Udp &&other) noexcept;

    ~Udp();

    struct Self;
    Self *operator->() noexcept;

    struct RecvFlags {
        /// Packet is partial (truncated).
        bool partial;

        /// Packet came from a recvmmsg batch (Linux).
        bool mmsg_chunk;

        constexpr RecvFlags(bool partial = false, bool mmsg_chunk = false) : partial(partial), mmsg_chunk(mmsg_chunk) {}
    };

    struct RecvResult {
        std::string data;
        std::string addr;
        i32 port = 0;
        RecvFlags flags;
    };

    struct Endpoint {
        std::string addr;
        i32 port = 0;
    };

    /// Multicast Membership operation.
    enum class Membership {
        JOIN, // join multicast group
        LEAVE // LEAVE multicast group
    };

    struct CreateOptions {
        /// Restrict socket to IPv6 only (ignore IPv4-mapped addresses).
        bool ipv6_only;

        /// Enable recvmmsg batching when supported.
        bool recvmmsg;

        constexpr CreateOptions(bool ipv6_only = false, bool recvmmsg = false) : ipv6_only(ipv6_only), recvmmsg(recvmmsg) {}
    };

    struct BindOptions {
        /// Restrict socket to IPv6 only (ignore IPv4-mapped addresses).
        bool ipv6_only;

        /// Enable SO_REUSEADDR if supported.
        bool reuse_addr;

        /// Enable SO_REUSEPORT if supported.
        bool reuse_port;

        constexpr BindOptions(bool ipv6_only = false, bool reuse_addr = false, bool reuse_port = false)
            : ipv6_only(ipv6_only), reuse_addr(reuse_addr), reuse_port(reuse_port) {}
    };

    static Result<Udp> create(EventLoop &loop = EventLoop::current());

    static Result<Udp> create(CreateOptions options = CreateOptions{}, EventLoop &loop = EventLoop::current());

    static Result<Udp> open(i32 fd, EventLoop &loop = EventLoop::current());

    Error bind(std::string_view host, i32 port, BindOptions options = BindOptions{});

    Error connect(std::string_view host, i32 port);

    Error disconnect();

    Task<void, Error> send(std::span<const char> data, std::string_view host, i32 port);

    Task<void, Error> send(std::span<const char> data);

    Error try_send(std::span<const char> data, std::string_view host, i32 port);

    Error try_send(std::span<const char> data);

    Result<Endpoint> getsockname() const;

    Result<Endpoint> getpeername() const;

    Error set_membership(std::string_view multicast_addr, std::string_view interface_addr, Membership m);

    Error set_source_membership(std::string_view multicast_addr, std::string_view interface_addr, std::string_view source_addr,
                                Membership m);

    Error set_multicast_loop(bool on);

    Error set_multicast_ttl(i32 ttl);

    Error set_multicast_interface(std::string_view interface_addr);

    Error set_broadcast(bool on);

    Error set_ttl(i32 ttl);

    bool using_recvmmsg() const;

    usize send_queue_size() const;

    usize send_queue_count() const;

    Error stop_recv();

    Task<RecvResult, Error> recv();

private:
    explicit Udp(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

} // namespace lighter
