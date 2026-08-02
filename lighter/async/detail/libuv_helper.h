#pragma once

#include <contracts>
#include <concepts>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include <uv.h>

#include <lighter/types.hpp>
#include <lighter/utils/type_traits.h>
#include <lighter/async/runtime/node.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#if defined(NDEBUG)
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define ALWAYS_INLINE inline
#endif
#else
#define ALWAYS_INLINE inline
#endif

namespace lighter { namespace uv {

template <typename T>
using bare_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
concept handle_like = is_one_of<bare_t<T>, uv_handle_t, uv_stream_t, uv_tcp_t, uv_pipe_t, uv_udp_t, uv_poll_t, uv_timer_t, uv_idle_t,
                                uv_prepare_t, uv_check_t, uv_process_t, uv_async_t>;

template <typename T>
concept stream_like = is_one_of<bare_t<T>, uv_stream_t, uv_tcp_t, uv_pipe_t>;

template <typename T>
concept req_like = is_one_of<bare_t<T>, uv_req_t, uv_fs_t, uv_work_t, uv_write_t, uv_udp_send_t, uv_connect_t>;

template <handle_like H>
ALWAYS_INLINE uv_handle_t *as_handle(H &handle) noexcept {
    if constexpr (std::same_as<bare_t<H>, uv_handle_t>) {
        return &handle;
    } else {
        return reinterpret_cast<uv_handle_t *>(&handle);
    }
}

template <handle_like H>
ALWAYS_INLINE const uv_handle_t *as_handle(const H &handle) noexcept {
    if constexpr (std::same_as<bare_t<H>, uv_handle_t>) {
        return &handle;
    } else {
        return reinterpret_cast<const uv_handle_t *>(&handle);
    }
}

template <stream_like S>
ALWAYS_INLINE uv_stream_t *as_stream(S &stream) noexcept {
    if constexpr (std::same_as<bare_t<S>, uv_stream_t>) {
        return &stream;
    } else {
        return reinterpret_cast<uv_stream_t *>(&stream);
    }
}

template <stream_like S>
ALWAYS_INLINE const uv_stream_t *as_stream(const S &stream) noexcept {
    if constexpr (std::same_as<bare_t<S>, uv_stream_t>) {
        return &stream;
    } else {
        return reinterpret_cast<const uv_stream_t *>(&stream);
    }
}

template <typename StatusT>
ALWAYS_INLINE Error status_to_error(StatusT status) noexcept {
    if (static_cast<i64>(status) < 0) {
        return Error(static_cast<i32>(status));
    }
    return {};
}

template <handle_like H>
ALWAYS_INLINE bool is_active(const H &handle) noexcept {
    return ::uv_is_active(as_handle(handle)) != 0;
}

/// Whether the handle is referenced, i.e. currently counts as work that keeps
/// uv_run() from returning.
template <handle_like H>
ALWAYS_INLINE bool has_ref(const H &handle) noexcept {
    return ::uv_has_ref(as_handle(handle)) != 0;
}

template <stream_like S>
ALWAYS_INLINE bool is_readable(const S &stream) noexcept {
    return ::uv_is_readable(as_stream(stream)) != 0;
}

template <stream_like S>
ALWAYS_INLINE bool is_writable(const S &stream) noexcept {
    return ::uv_is_writable(as_stream(stream)) != 0;
}

template <handle_like H>
ALWAYS_INLINE bool is_closing(const H &handle) noexcept {
    return ::uv_is_closing(as_handle(handle)) != 0;
}

template <handle_like H>
ALWAYS_INLINE void ref(H &handle) noexcept {
    ::uv_ref(as_handle(handle));
}

template <handle_like H>
ALWAYS_INLINE void unref(H &handle) noexcept {
    ::uv_unref(as_handle(handle));
}

template <handle_like H>
ALWAYS_INLINE void close(H &handle, uv_close_cb cb = nullptr) noexcept {
    ::uv_close(as_handle(handle), cb);
}

template <req_like R>
ALWAYS_INLINE Error cancel(R &req) noexcept {
    // Errors: UV_EINVAL (req type not cancellable), UV_EBUSY (already running/done).
    return status_to_error(::uv_cancel(reinterpret_cast<uv_req_t *>(&req)));
}

ALWAYS_INLINE Error loop_init(uv_loop_t &loop) noexcept {
    // Errors: UV_ENOMEM and platform init failures.
    return status_to_error(::uv_loop_init(&loop));
}

ALWAYS_INLINE Error loop_close(uv_loop_t &loop) noexcept {
    // Errors: UV_EBUSY when active handles/requests remain.
    return status_to_error(::uv_loop_close(&loop));
}

ALWAYS_INLINE i32 run(uv_loop_t &loop, uv_run_mode mode) noexcept { return ::uv_run(&loop, mode); }

ALWAYS_INLINE void stop(uv_loop_t &loop) noexcept { ::uv_stop(&loop); }

ALWAYS_INLINE void walk(uv_loop_t &loop, uv_walk_cb cb, void *arg) noexcept pre(cb != nullptr) { ::uv_walk(&loop, cb, arg); }

ALWAYS_INLINE void idle_init(uv_loop_t &loop, uv_idle_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_idle_init(&loop, &handle); }

ALWAYS_INLINE void idle_start(uv_idle_t &handle, uv_idle_cb cb) noexcept pre(cb != nullptr) {
    [[maybe_unused]] i32 rc = ::uv_idle_start(&handle, cb);
    // for cb != nullptr, idle start always succeeds
}

ALWAYS_INLINE void idle_stop(uv_idle_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_idle_stop(&handle); }

ALWAYS_INLINE void async_init(uv_loop_t &loop, uv_async_t &handle, uv_async_cb cb) noexcept {
    [[maybe_unused]] i32 rc = ::uv_async_init(&loop, &handle, cb);
    lighter::check(rc == 0, "uv_async_init failed");
}

ALWAYS_INLINE void async_send(uv_async_t &handle) noexcept {
    [[maybe_unused]] i32 rc = ::uv_async_send(&handle);
    lighter::check(rc == 0, "uv_async_send failed");
}

ALWAYS_INLINE void prepare_init(uv_loop_t &loop, uv_prepare_t &handle) noexcept {
    [[maybe_unused]] i32 rc = ::uv_prepare_init(&loop, &handle);
}

ALWAYS_INLINE void prepare_start(uv_prepare_t &handle, uv_prepare_cb cb) noexcept pre(cb != nullptr) {
    [[maybe_unused]] i32 rc = ::uv_prepare_start(&handle, cb);
    // for cb != nullptr, prepare start always succeeds
}

ALWAYS_INLINE void prepare_stop(uv_prepare_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_prepare_stop(&handle); }

ALWAYS_INLINE void check_init(uv_loop_t &loop, uv_check_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_check_init(&loop, &handle); }

ALWAYS_INLINE void check_start(uv_check_t &handle, uv_check_cb cb) noexcept pre(cb != nullptr) {
    [[maybe_unused]] i32 rc = ::uv_check_start(&handle, cb);
    // for cb != nullptr, check start always succeeds
}

ALWAYS_INLINE void check_stop(uv_check_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_check_stop(&handle); }

ALWAYS_INLINE void timer_init(uv_loop_t &loop, uv_timer_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_timer_init(&loop, &handle); }

ALWAYS_INLINE void timer_start(uv_timer_t &handle, uv_timer_cb cb, u64 timeout, u64 repeat) noexcept
    pre(cb != nullptr && !is_closing(handle)) {
    [[maybe_unused]] i32 rc = ::uv_timer_start(&handle, cb, timeout, repeat);
}

ALWAYS_INLINE void timer_stop(uv_timer_t &handle) noexcept { [[maybe_unused]] i32 rc = ::uv_timer_stop(&handle); }

ALWAYS_INLINE Error poll_init_socket(uv_loop_t &loop, uv_poll_t &handle, uv_os_sock_t socket) noexcept {
    return status_to_error(::uv_poll_init_socket(&loop, &handle, socket));
}

ALWAYS_INLINE Error poll_start(uv_poll_t &handle, i32 events, uv_poll_cb cb) noexcept pre(cb != nullptr) {
    return status_to_error(::uv_poll_start(&handle, events, cb));
}

ALWAYS_INLINE Error poll_stop(uv_poll_t &handle) noexcept { return status_to_error(::uv_poll_stop(&handle)); }

ALWAYS_INLINE Error queue_work(uv_loop_t &loop, uv_work_t &req, uv_work_cb work_cb, uv_after_work_cb after_work_cb) noexcept
    pre(work_cb != nullptr) {
    // Errors: UV_EINVAL for invalid arguments.
    return status_to_error(::uv_queue_work(&loop, &req, work_cb, after_work_cb));
}

template <stream_like S>
ALWAYS_INLINE Error read_start(S &stream, uv_alloc_cb alloc_cb, uv_read_cb read_cb) noexcept pre(alloc_cb != nullptr)
    pre(read_cb != nullptr) {
    // Errors: UV_EINVAL / UV_EALREADY / UV_ENOTCONN
    return status_to_error(::uv_read_start(as_stream(stream), alloc_cb, read_cb));
}

template <stream_like S>
ALWAYS_INLINE void read_stop(S &stream) noexcept {
    [[maybe_unused]] i32 rc = ::uv_read_stop(as_stream(stream));
    // read_stop always succeeds, non zero rc will only mean it's still pending
}

template <stream_like S>
ALWAYS_INLINE Error write(uv_write_t &req, S &stream, std::span<const uv_buf_t> bufs, uv_write_cb cb) noexcept pre(!bufs.empty()) {
    // Errors: stream state/fd/write precondition failures.
    return status_to_error(::uv_write(&req, as_stream(stream), bufs.data(), static_cast<u32>(bufs.size()), cb));
}

template <stream_like Server, stream_like Client>
ALWAYS_INLINE Error accept(Server &server, Client &client) noexcept {
    return status_to_error(::uv_accept(as_stream(server), as_stream(client)));
}

template <stream_like S>
ALWAYS_INLINE Error listen(S &stream, i32 backlog, uv_connection_cb cb) noexcept pre(cb != nullptr) {
    return status_to_error(::uv_listen(as_stream(stream), backlog, cb));
}

ALWAYS_INLINE Error pipe_init(uv_loop_t &loop, uv_pipe_t &handle, i32 ipc) noexcept {
    return status_to_error(::uv_pipe_init(&loop, &handle, ipc));
}

ALWAYS_INLINE Error pipe_open(uv_pipe_t &handle, uv_file fd) noexcept { return status_to_error(::uv_pipe_open(&handle, fd)); }

ALWAYS_INLINE Error pipe_bind2(uv_pipe_t &handle, const char *name, usize namelen, u32 flags) noexcept pre(name != nullptr)
    pre(namelen > 0) {
    return status_to_error(::uv_pipe_bind2(&handle, name, namelen, flags));
}

ALWAYS_INLINE Error pipe_connect2(uv_connect_t &req, uv_pipe_t &handle, const char *name, usize namelen, u32 flags,
                                  uv_connect_cb cb) noexcept pre(cb != nullptr) pre(name != nullptr) pre(namelen > 0) {
    return status_to_error(::uv_pipe_connect2(&req, &handle, name, namelen, flags, cb));
}

ALWAYS_INLINE Error tcp_init(uv_loop_t &loop, uv_tcp_t &handle) noexcept { return status_to_error(::uv_tcp_init(&loop, &handle)); }

ALWAYS_INLINE Error tcp_open(uv_tcp_t &handle, uv_os_sock_t sock) noexcept { return status_to_error(::uv_tcp_open(&handle, sock)); }

ALWAYS_INLINE Error tcp_bind(uv_tcp_t &handle, const sockaddr *addr, u32 flags) noexcept pre(addr != nullptr) {
    return status_to_error(::uv_tcp_bind(&handle, addr, flags));
}

ALWAYS_INLINE Error tcp_connect(uv_connect_t &req, uv_tcp_t &handle, const sockaddr *addr, uv_connect_cb cb) noexcept pre(addr != nullptr)
    pre(cb != nullptr) {
    return status_to_error(::uv_tcp_connect(&req, &handle, addr, cb));
}

template <stream_like S>
ALWAYS_INLINE Result<usize> try_write(S &stream, std::span<const uv_buf_t> bufs) noexcept pre(!bufs.empty()) {
    [[maybe_unused]] i32 rc = ::uv_try_write(as_stream(stream), bufs.data(), static_cast<u32>(bufs.size()));
    if (rc < 0) {
        return outcome_error(Error(rc));
    }
    return static_cast<usize>(rc);
}

template <stream_like S>
ALWAYS_INLINE Error stream_set_blocking(S &stream, bool enabled) noexcept {
    return status_to_error(::uv_stream_set_blocking(as_stream(stream), enabled ? 1 : 0));
}

ALWAYS_INLINE Error udp_init(uv_loop_t &loop, uv_udp_t &handle) noexcept { return status_to_error(::uv_udp_init(&loop, &handle)); }

ALWAYS_INLINE Error udp_init_ex(uv_loop_t &loop, uv_udp_t &handle, u32 flags) noexcept {
    return status_to_error(::uv_udp_init_ex(&loop, &handle, flags));
}

ALWAYS_INLINE Error udp_open(uv_udp_t &handle, uv_os_sock_t sock) noexcept { return status_to_error(::uv_udp_open(&handle, sock)); }

ALWAYS_INLINE Error udp_bind(uv_udp_t &handle, const sockaddr *addr, u32 flags) noexcept pre(addr != nullptr) {
    return status_to_error(::uv_udp_bind(&handle, addr, flags));
}

ALWAYS_INLINE Error udp_connect(uv_udp_t &handle, const sockaddr *addr) noexcept {
    return status_to_error(::uv_udp_connect(&handle, addr));
}

ALWAYS_INLINE Error udp_recv_start(uv_udp_t &handle, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb) noexcept pre(alloc_cb != nullptr)
    pre(recv_cb != nullptr) {
    return status_to_error(::uv_udp_recv_start(&handle, alloc_cb, recv_cb));
}

ALWAYS_INLINE void udp_recv_stop(uv_udp_t &handle) noexcept pre(handle.type == UV_UDP) { ::uv_udp_recv_stop(&handle); }

ALWAYS_INLINE Error udp_send(uv_udp_send_t &req, uv_udp_t &handle, std::span<const uv_buf_t> bufs, const sockaddr *addr,
                             uv_udp_send_cb cb) noexcept pre(!bufs.empty()) {
    return status_to_error(::uv_udp_send(&req, &handle, bufs.data(), static_cast<u32>(bufs.size()), addr, cb));
}

ALWAYS_INLINE Result<usize> udp_try_send(uv_udp_t &handle, std::span<const uv_buf_t> bufs, const sockaddr *addr) noexcept
    pre(!bufs.empty()) {
    [[maybe_unused]] i32 rc = ::uv_udp_try_send(&handle, bufs.data(), static_cast<u32>(bufs.size()), addr);
    if (rc < 0) {
        return outcome_error(Error(rc));
    }
    return static_cast<usize>(rc);
}

ALWAYS_INLINE Error udp_getsockname(const uv_udp_t &handle, sockaddr &name, i32 &namelen) noexcept {
    return status_to_error(::uv_udp_getsockname(&handle, &name, &namelen));
}

ALWAYS_INLINE Error udp_getpeername(const uv_udp_t &handle, sockaddr &name, i32 &namelen) noexcept {
    return status_to_error(::uv_udp_getpeername(&handle, &name, &namelen));
}

ALWAYS_INLINE Error udp_set_membership(uv_udp_t &handle, const char *multicast_addr, const char *interface_addr,
                                       uv_membership membership) noexcept {
    return status_to_error(::uv_udp_set_membership(&handle, multicast_addr, interface_addr, membership));
}

ALWAYS_INLINE Error udp_set_source_membership(uv_udp_t &handle, const char *multicast_addr, const char *interface_addr,
                                              const char *source_addr, uv_membership membership) noexcept {
    return status_to_error(::uv_udp_set_source_membership(&handle, multicast_addr, interface_addr, source_addr, membership));
}

ALWAYS_INLINE Error udp_set_multicast_loop(uv_udp_t &handle, bool on) noexcept {
    return status_to_error(::uv_udp_set_multicast_loop(&handle, on ? 1 : 0));
}

ALWAYS_INLINE Error udp_set_multicast_ttl(uv_udp_t &handle, i32 ttl) noexcept {
    return status_to_error(::uv_udp_set_multicast_ttl(&handle, ttl));
}

ALWAYS_INLINE Error udp_set_multicast_interface(uv_udp_t &handle, const char *interface_addr) noexcept {
    return status_to_error(::uv_udp_set_multicast_interface(&handle, interface_addr));
}

ALWAYS_INLINE Error udp_set_broadcast(uv_udp_t &handle, bool on) noexcept {
    return status_to_error(::uv_udp_set_broadcast(&handle, on ? 1 : 0));
}

ALWAYS_INLINE Error udp_set_ttl(uv_udp_t &handle, i32 ttl) noexcept { return status_to_error(::uv_udp_set_ttl(&handle, ttl)); }

ALWAYS_INLINE bool udp_using_recvmmsg(const uv_udp_t &handle) noexcept { return ::uv_udp_using_recvmmsg(&handle) != 0; }

ALWAYS_INLINE usize udp_get_send_queue_size(const uv_udp_t &handle) noexcept { return ::uv_udp_get_send_queue_size(&handle); }

ALWAYS_INLINE usize udp_get_send_queue_count(const uv_udp_t &handle) noexcept { return ::uv_udp_get_send_queue_count(&handle); }

/// uv_spawn internally registers a SIGCHLD handler in a process-global
/// red-black tree that is NOT thread-safe.  Serialise all spawn calls
/// so that concurrent Event-loops on different threads do not race.
inline Error spawn(uv_loop_t &loop, uv_process_t &process, const uv_process_options_t &options) noexcept pre(options.file != nullptr) {
    static std::mutex spawn_mutex;
    std::lock_guard lock(spawn_mutex);
    return status_to_error(::uv_spawn(&loop, &process, &options));
}

ALWAYS_INLINE Error process_kill(uv_process_t &process, i32 signum) noexcept {
    return status_to_error(::uv_process_kill(&process, signum));
}

ALWAYS_INLINE uv_buf_t buf_init(char *base, u32 len) noexcept { return ::uv_buf_init(base, len); }

ALWAYS_INLINE Error ip4_addr(const char *ip, i32 port, sockaddr_in &out) noexcept pre(ip != nullptr) {
    return status_to_error(::uv_ip4_addr(ip, port, &out));
}

ALWAYS_INLINE Error ip6_addr(const char *ip, i32 port, sockaddr_in6 &out) noexcept pre(ip != nullptr) {
    return status_to_error(::uv_ip6_addr(ip, port, &out));
}

ALWAYS_INLINE Error ip4_name(const sockaddr_in &src, char *dst, usize size) noexcept pre(dst != nullptr) pre(size > 0) {
    return status_to_error(::uv_ip4_name(&src, dst, size));
}

ALWAYS_INLINE Error ip6_name(const sockaddr_in6 &src, char *dst, usize size) noexcept pre(dst != nullptr) pre(size > 0) {
    return status_to_error(::uv_ip6_name(&src, dst, size));
}

ALWAYS_INLINE std::string_view strerror(i32 code) noexcept {
    const char *msg = ::uv_strerror(code);
    return msg == nullptr ? std::string_view{} : std::string_view(msg);
}

ALWAYS_INLINE void fs_req_cleanup(uv_fs_t &req) noexcept { ::uv_fs_req_cleanup(&req); }

ALWAYS_INLINE Error fs_unlink(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_unlink(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_mkdir(uv_loop_t &loop, uv_fs_t &req, const char *path, i32 mode, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_mkdir(&loop, &req, path, mode, cb));
}

ALWAYS_INLINE Error fs_stat(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_stat(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_copyfile(uv_loop_t &loop, uv_fs_t &req, const char *path, const char *new_path, i32 flags, uv_fs_cb cb) noexcept
    pre(path != nullptr) pre(new_path != nullptr) {
    return status_to_error(::uv_fs_copyfile(&loop, &req, path, new_path, flags, cb));
}

ALWAYS_INLINE Error fs_mkdtemp(uv_loop_t &loop, uv_fs_t &req, const char *tpl, uv_fs_cb cb) noexcept pre(tpl != nullptr) {
    return status_to_error(::uv_fs_mkdtemp(&loop, &req, tpl, cb));
}

ALWAYS_INLINE Error fs_mkstemp(uv_loop_t &loop, uv_fs_t &req, const char *tpl, uv_fs_cb cb) noexcept pre(tpl != nullptr) {
    return status_to_error(::uv_fs_mkstemp(&loop, &req, tpl, cb));
}

ALWAYS_INLINE Error fs_rmdir(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_rmdir(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_scandir(uv_loop_t &loop, uv_fs_t &req, const char *path, i32 flags, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_scandir(&loop, &req, path, flags, cb));
}

ALWAYS_INLINE Error fs_scandir_next(uv_fs_t &req, uv_dirent_t &ent) noexcept { return Error(::uv_fs_scandir_next(&req, &ent)); }

ALWAYS_INLINE Error fs_opendir(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_opendir(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_readdir(uv_loop_t &loop, uv_fs_t &req, uv_dir_t &dir, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_readdir(&loop, &req, &dir, cb));
}

ALWAYS_INLINE Error fs_closedir(uv_loop_t &loop, uv_fs_t &req, uv_dir_t &dir, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_closedir(&loop, &req, &dir, cb));
}

ALWAYS_INLINE Error fs_fstat(uv_loop_t &loop, uv_fs_t &req, uv_file file, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_fstat(&loop, &req, file, cb));
}

ALWAYS_INLINE Error fs_lstat(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_lstat(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_rename(uv_loop_t &loop, uv_fs_t &req, const char *path, const char *new_path, uv_fs_cb cb) noexcept
    pre(path != nullptr) pre(new_path != nullptr) {
    return status_to_error(::uv_fs_rename(&loop, &req, path, new_path, cb));
}

ALWAYS_INLINE Error fs_fsync(uv_loop_t &loop, uv_fs_t &req, uv_file file, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_fsync(&loop, &req, file, cb));
}

ALWAYS_INLINE Error fs_fdatasync(uv_loop_t &loop, uv_fs_t &req, uv_file file, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_fdatasync(&loop, &req, file, cb));
}

ALWAYS_INLINE Error fs_ftruncate(uv_loop_t &loop, uv_fs_t &req, uv_file file, i64 off, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_ftruncate(&loop, &req, file, off, cb));
}

ALWAYS_INLINE Error fs_sendfile(uv_loop_t &loop, uv_fs_t &req, uv_file out_file, uv_file in_file, i64 in_offset, usize length,
                                uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_sendfile(&loop, &req, out_file, in_file, in_offset, length, cb));
}

ALWAYS_INLINE Error fs_access(uv_loop_t &loop, uv_fs_t &req, const char *path, i32 mode, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_access(&loop, &req, path, mode, cb));
}

ALWAYS_INLINE Error fs_chmod(uv_loop_t &loop, uv_fs_t &req, const char *path, i32 mode, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_chmod(&loop, &req, path, mode, cb));
}

ALWAYS_INLINE Error fs_utime(uv_loop_t &loop, uv_fs_t &req, const char *path, f64 atime, f64 mtime, uv_fs_cb cb) noexcept
    pre(path != nullptr) {
    return status_to_error(::uv_fs_utime(&loop, &req, path, atime, mtime, cb));
}

ALWAYS_INLINE Error fs_futime(uv_loop_t &loop, uv_fs_t &req, uv_file file, f64 atime, f64 mtime, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_futime(&loop, &req, file, atime, mtime, cb));
}

ALWAYS_INLINE Error fs_lutime(uv_loop_t &loop, uv_fs_t &req, const char *path, f64 atime, f64 mtime, uv_fs_cb cb) noexcept
    pre(path != nullptr) {
    return status_to_error(::uv_fs_lutime(&loop, &req, path, atime, mtime, cb));
}

ALWAYS_INLINE Error fs_link(uv_loop_t &loop, uv_fs_t &req, const char *path, const char *new_path, uv_fs_cb cb) noexcept
    pre(path != nullptr) pre(new_path != nullptr) {
    return status_to_error(::uv_fs_link(&loop, &req, path, new_path, cb));
}

ALWAYS_INLINE Error fs_symlink(uv_loop_t &loop, uv_fs_t &req, const char *path, const char *new_path, i32 flags, uv_fs_cb cb) noexcept
    pre(path != nullptr) pre(new_path != nullptr) {
    return status_to_error(::uv_fs_symlink(&loop, &req, path, new_path, flags, cb));
}

ALWAYS_INLINE Error fs_readlink(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_readlink(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_realpath(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_realpath(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_fchmod(uv_loop_t &loop, uv_fs_t &req, uv_file file, i32 mode, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_fchmod(&loop, &req, file, mode, cb));
}

ALWAYS_INLINE Error fs_chown(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb) noexcept
    pre(path != nullptr) {
    return status_to_error(::uv_fs_chown(&loop, &req, path, uid, gid, cb));
}

ALWAYS_INLINE Error fs_fchown(uv_loop_t &loop, uv_fs_t &req, uv_file file, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_fchown(&loop, &req, file, uid, gid, cb));
}

ALWAYS_INLINE Error fs_lchown(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb) noexcept
    pre(path != nullptr) {
    return status_to_error(::uv_fs_lchown(&loop, &req, path, uid, gid, cb));
}

ALWAYS_INLINE Error fs_statfs(uv_loop_t &loop, uv_fs_t &req, const char *path, uv_fs_cb cb) noexcept pre(path != nullptr) {
    return status_to_error(::uv_fs_statfs(&loop, &req, path, cb));
}

ALWAYS_INLINE Error fs_open(uv_loop_t &loop, uv_fs_t &req, const char *path, i32 flags, i32 mode, uv_fs_cb cb) noexcept
    pre(path != nullptr) {
    return status_to_error(::uv_fs_open(&loop, &req, path, flags, mode, cb));
}

ALWAYS_INLINE Error fs_read(uv_loop_t &loop, uv_fs_t &req, uv_file file, const uv_buf_t bufs[], u32 nbufs, i64 offset,
                            uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_read(&loop, &req, file, bufs, nbufs, offset, cb));
}

ALWAYS_INLINE Error fs_write(uv_loop_t &loop, uv_fs_t &req, uv_file file, const uv_buf_t bufs[], u32 nbufs, i64 offset,
                             uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_write(&loop, &req, file, bufs, nbufs, offset, cb));
}

ALWAYS_INLINE Error fs_close(uv_loop_t &loop, uv_fs_t &req, uv_file file, uv_fs_cb cb) noexcept {
    return status_to_error(::uv_fs_close(&loop, &req, file, cb));
}

// --- System / OS utility wrappers ---

ALWAYS_INLINE Error resident_set_memory(usize &rss) noexcept { return status_to_error(::uv_resident_set_memory(&rss)); }

ALWAYS_INLINE Error getrusage(uv_rusage_t &usage) noexcept { return status_to_error(::uv_getrusage(&usage)); }

ALWAYS_INLINE Error cpu_info(uv_cpu_info_t *&infos, i32 &count) noexcept { return status_to_error(::uv_cpu_info(&infos, &count)); }

ALWAYS_INLINE void free_cpu_info(uv_cpu_info_t *infos, i32 count) noexcept { ::uv_free_cpu_info(infos, count); }

ALWAYS_INLINE Error os_uname(uv_utsname_t &buf) noexcept { return status_to_error(::uv_os_uname(&buf)); }

ALWAYS_INLINE Error os_gethostname(char *buf, usize &size) noexcept pre(buf != nullptr) {
    return status_to_error(::uv_os_gethostname(buf, &size));
}

ALWAYS_INLINE Error uptime(f64 &value) noexcept { return status_to_error(::uv_uptime(&value)); }

ALWAYS_INLINE Error os_homedir(char *buf, usize &size) noexcept pre(buf != nullptr) { return status_to_error(::uv_os_homedir(buf, &size)); }

ALWAYS_INLINE Error os_tmpdir(char *buf, usize &size) noexcept pre(buf != nullptr) { return status_to_error(::uv_os_tmpdir(buf, &size)); }

ALWAYS_INLINE Error os_getpriority(uv_pid_t pid, i32 &priority) noexcept { return status_to_error(::uv_os_getpriority(pid, &priority)); }

ALWAYS_INLINE Error os_setpriority(uv_pid_t pid, i32 priority) noexcept { return status_to_error(::uv_os_setpriority(pid, priority)); }

ALWAYS_INLINE uv_pid_t os_getpid() noexcept { return ::uv_os_getpid(); }

ALWAYS_INLINE u64 get_total_memory() noexcept { return ::uv_get_total_memory(); }

ALWAYS_INLINE u64 get_free_memory() noexcept { return ::uv_get_free_memory(); }

ALWAYS_INLINE u64 get_available_memory() noexcept { return ::uv_get_available_memory(); }

ALWAYS_INLINE u64 get_constrained_memory() noexcept { return ::uv_get_constrained_memory(); }

ALWAYS_INLINE u32 available_parallelism() noexcept { return ::uv_available_parallelism(); }

// --- Synchronous fs wrappers (no Event loop needed) ---

ALWAYS_INLINE i32 fs_open_sync(uv_fs_t &req, const char *path, i32 flags, i32 mode) noexcept pre(path != nullptr) {
    return ::uv_fs_open(nullptr, &req, path, flags, mode, nullptr);
}

ALWAYS_INLINE i32 fs_read_sync(uv_fs_t &req, uv_file file, const uv_buf_t bufs[], u32 nbufs, i64 offset) noexcept {
    return ::uv_fs_read(nullptr, &req, file, bufs, nbufs, offset, nullptr);
}

ALWAYS_INLINE i32 fs_write_sync(uv_fs_t &req, uv_file file, const uv_buf_t bufs[], u32 nbufs, i64 offset) noexcept {
    return ::uv_fs_write(nullptr, &req, file, bufs, nbufs, offset, nullptr);
}

ALWAYS_INLINE i32 fs_close_sync(uv_fs_t &req, uv_file file) noexcept { return ::uv_fs_close(nullptr, &req, file, nullptr); }

#undef ALWAYS_INLINE

struct ResolvedAddr {
    sockaddr_storage storage{};
    i32 family = AF_UNSPEC;
};

inline Result<ResolvedAddr> resolve_addr(std::string_view host, i32 port) {
    std::string host_storage(host);
    ResolvedAddr out{};

    auto &out6 = *reinterpret_cast<sockaddr_in6 *>(&out.storage);
    auto err6 = uv::ip6_addr(host_storage.c_str(), port, out6);
    if (!err6) {
        out.family = AF_INET6;
        return out;
    }

    auto &out4 = *reinterpret_cast<sockaddr_in *>(&out.storage);
    auto err4 = uv::ip4_addr(host_storage.c_str(), port, out4);
    if (!err4) {
        out.family = AF_INET;
        return out;
    }

    return outcome_error(Error::k_invalid_argument);
}

/// Tracks handles that were force-closed during EventLoop teardown.
///
/// This is a fallback path for late owner destruction: the normal contract is
/// that Task/handle owners should be destroyed before their EventLoop goes
/// away, so they can issue uv_close() themselves and receive their delete
/// callback through the loop. Only when that contract is violated do we record
/// a handle here so a later owner destruction can safely release the wrapper.
///
/// Storage is thread-local because libuv handles are thread-affine and a single
/// thread may create and tear down multiple loops sequentially.
struct LoopCloseFallback {
    using registry_type = std::unordered_set<const uv_handle_t *>;

    static void mark(const uv_handle_t *handle) { handles().insert(handle); }

    static bool take(const uv_handle_t *handle) { return handles().erase(handle) != 0; }

private:
    static registry_type &handles() {
        static thread_local registry_type late_closed_handles;
        return late_closed_handles;
    }
};

template <typename Derived, typename Handle>
struct UniqueHandleImpl {
protected:
    UniqueHandleImpl() = default;
    ~UniqueHandleImpl() = default;

public:
    using pointer = UniqueHandle<Derived>;

    bool initialized() const noexcept {
        auto *self = static_cast<const Derived *>(this);
        auto *h = uv::as_handle(self->handle);
        return h->loop != nullptr && h->type != UV_UNKNOWN_HANDLE;
    }

    static pointer make() {
        auto self = pointer(new Derived());
        auto *h = uv::as_handle(self->handle);
        *h = {};
        h->data = self.get();
        return self;
    }

    /// Releases the wrapper around a libuv handle.
    ///
    /// Normal path: the owner is destroyed while the Event loop is still alive,
    /// so we issue uv_close() with a delete callback and let libuv release the
    /// wrapper once the close finishes on the loop.
    ///
    /// Fallback path: EventLoop teardown already force-closed the handle and
    /// recorded it in uv::LoopCloseFallback. In that case the wrapper may be
    /// destroyed later and we reclaim it directly here.
    ///
    /// Any other observed closing state is unexpected and indicates a lifetime
    /// bug: the handle is closing, but not through the recognized loop-teardown
    /// fallback.
    static void destroy(Derived *self) noexcept {
        if (!self) {
            return;
        }

        if (!self->initialized()) {
            delete self;
            return;
        }

        auto &h = self->handle;
        h.data = self;
        if (uv::is_closing(h)) {
            const bool closed_by_loop_cleanup = uv::LoopCloseFallback::take(uv::as_handle(h));
            if (!closed_by_loop_cleanup) {
                // This is a lifetime bug: the handle is closing, but not through
                // the recognized loop-teardown fallback.
                contract_assert(false);
                return;
            }

            delete self;
            return;
        }

        uv::close(h, [](uv_handle_t *handle) {
            auto *self = static_cast<Derived *>(handle->data);
            delete self;
        });
    }
};

template <typename Derived, typename Handle>
using handle = UniqueHandleImpl<Derived, Handle>;

}} // namespace lighter::uv
