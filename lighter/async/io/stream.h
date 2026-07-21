#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <lighter/scalar_types.hpp>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

class EventLoop;

template <typename Stream>
class Acceptor;

/// Stream handle classification for file descriptors.
enum class HandleType { UNKNOWN,
                        FILE,
                        TTY,
                        PIPE,
                        TCP,
                        UDP };

/// Guess the handle type for a file descriptor.
HandleType guess_handle(i32 fd);

/// Base Stream wrapper for PIPE, TCP, and Console handles.
class Stream {
public:
    Stream() noexcept;

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    Stream(Stream &&other) noexcept;
    Stream &operator=(Stream &&other) noexcept;

    ~Stream();

    struct Self;
    Self *operator->() noexcept;

    /// Raw libuv handle pointer, or nullptr if invalid.
    void *handle() noexcept;

    /// Raw libuv handle pointer, or nullptr if invalid.
    const void *handle() const noexcept;

    /// Read available data into a std::string; waits for at least one read if empty.
    Task<std::string, Error> read();

    /// Read up to dst.size() bytes into dst; returns bytes read, 0 on EOF, or an Error.
    Task<usize, Error> read_some(std::span<char> dst);

    using Chunk = std::span<const char>;

    /// Read a chunk view into the internal buffer; call consume() after processing.
    Task<Chunk, Error> read_chunk();

    /// Consume bytes from the internal buffer.
    void consume(usize n);

    /// Stop active reads and abort any pending read waiter.
    void stop();

    /// Write data to the Stream; only one writer at a time.
    Task<void, Error> write(std::span<const char> data);

    /// Try a non-blocking write; returns bytes written or Error.
    Result<usize> try_write(std::span<const char> data);

    /// Check whether the Stream is readable.
    bool readable() const noexcept;

    /// Check whether the Stream is writable.
    bool writable() const noexcept;

    /// Enable or disable blocking I/O on the Stream.
    Error set_blocking(bool enabled);

protected:
    explicit Stream(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

template <typename Stream>
class Acceptor {
public:
    Acceptor() noexcept;

    Acceptor(const Acceptor &) = delete;
    Acceptor &operator=(const Acceptor &) = delete;

    Acceptor(Acceptor &&other) noexcept;
    Acceptor &operator=(Acceptor &&other) noexcept;

    ~Acceptor();

    struct Self;
    /// Internal access; null when invalid.
    Self *operator->() noexcept;

    /// Accept one connection; only one pending accept is allowed at a time.
    Task<Stream, Error> accept();

    /// Stop pending accept which will complete with Error::k_operation_aborted. If no accept is
    /// pending, the next accept() will complete with Error instead.
    Error stop();

private:
    friend class Pipe;
    friend class Tcp;

    explicit Acceptor(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

/// Pipe/socket wrapper (named Pipe on Windows, Unix domain socket on Unix).
class Pipe : public Stream {
public:
    Pipe() noexcept = default;

    using Acceptor = lighter::Acceptor<Pipe>;

    struct Options {
        /// Enable IPC handle passing.
        bool ipc = false;

        /// Do not truncate long Pipe names (UV_PIPE_NO_TRUNCATE when supported).
        bool no_truncate = false;

        /// Listen backlog size.
        i32 backlog = 128;

        constexpr Options(bool ipc = false, bool no_truncate = false, i32 backlog = 128) : ipc(ipc), no_truncate(no_truncate), backlog(backlog) {}
    };

    /// Wrap an existing file descriptor.
    static Result<Pipe> open(i32 fd,
                             Options opts = Options(),
                             EventLoop &loop = EventLoop::current());

    /// Connect to a named Pipe.
    static Task<Pipe, Error> connect(std::string_view name,
                                     Options opts = Options(),
                                     EventLoop &loop = EventLoop::current());

    /// Listen on a named Pipe.
    static Result<Acceptor> listen(std::string_view name,
                                   Options opts = Options(),
                                   EventLoop &loop = EventLoop::current());

    explicit Pipe(UniqueHandle<Self> self) noexcept;

private:
    friend class Process;

    static Result<Pipe> create(Options opts = Options(), EventLoop &loop = EventLoop::current());
};

/// TCP socket wrapper.
class Tcp : public Stream {
public:
    Tcp() noexcept = default;

    explicit Tcp(UniqueHandle<Self> self) noexcept;

    using Acceptor = lighter::Acceptor<Tcp>;

    struct Options {
        /// Restrict socket to IPv6 only (ignore IPv4-mapped addresses).
        bool ipv6_only = false;

        /// Enable SO_REUSEPORT when supported.
        bool reuse_port = false;

        /// Listen backlog size.
        i32 backlog = 128;

        constexpr Options(bool ipv6_only = false, bool reuse_port = false, i32 backlog = 128) : ipv6_only(ipv6_only), reuse_port(reuse_port), backlog(backlog) {}
    };

    /// Wrap an existing socket descriptor.
    static Result<Tcp> open(i32 fd, EventLoop &loop = EventLoop::current());

    /// Connect to a TCP host/port.
    static Task<Tcp, Error> connect(std::string_view host,
                                    i32 port,
                                    EventLoop &loop = EventLoop::current());

    /// Listen on a TCP host/port.
    static Result<Acceptor> listen(std::string_view host,
                                   i32 port,
                                   Options opts = Options(),
                                   EventLoop &loop = EventLoop::current());

    /// Query the local address/port of a listening Acceptor.
    static Result<i32> local_port(Acceptor &acc);
};

/// TTY/Console wrapper.
class Console : public Stream {
public:
    Console() noexcept = default;

    struct Winsize {
        /// Console width in columns.
        i32 width = 0;

        /// Console height in rows.
        i32 height = 0;
    };

    enum class Mode { NORMAL,
                      RAW,
                      IO,
                      RAW_VT };

    enum class VtermState { SUPPORTED,
                            UNSUPPORTED };

    struct Options {
        /// Whether the TTY is readable (stdin).
        bool readable = false;

        constexpr Options(bool readable = false) : readable(readable) {}
    };

    /// Wrap a Console file descriptor.
    static Result<Console> open(i32 fd,
                                Options opts = Options(),
                                EventLoop &loop = EventLoop::current());

    /// Set TTY/Console mode.
    Error set_mode(Mode value);

    /// Reset TTY/Console mode.
    static Error reset_mode();

    /// Fetch terminal dimensions.
    Result<Winsize> get_winsize() const;

    /// Set global virtual terminal processing state.
    static void set_vterm_state(VtermState state);

    /// Query global virtual terminal processing state.
    static Result<VtermState> get_vterm_state();

private:
    explicit Console(UniqueHandle<Self> self) noexcept;
};

} // namespace lighter
