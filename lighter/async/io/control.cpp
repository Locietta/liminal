#include "control.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <lighter/async/detail/native_event_queue.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace lighter {

namespace {

constexpr usize k_control_kind_count = 7;
constexpr usize k_max_control_sources = 8;

constexpr usize kind_index(ControlEventKind kind) noexcept { return static_cast<usize>(kind); }

static_assert(k_control_kind_count == kind_index(ControlEventKind::SHUTDOWN) + 1);
static_assert(k_control_kind_count <= 32);

constexpr u32 kind_bit(ControlEventKind kind) noexcept { return u32{1} << kind_index(kind); }

} // namespace

struct ControlEventSource::Self {
    std::shared_ptr<detail::NativeEventQueue<ControlEventKind>> delivery = std::make_shared<detail::NativeEventQueue<ControlEventKind>>();
    Relay relay;
    u32 watched = 0;
    isize registry_slot = -1;
    std::thread worker;

#ifdef _WIN32
    HANDLE ready = nullptr;
    HANDLE stop = nullptr;
    std::array<std::atomic<u32>, k_control_kind_count> pending{};
#else
    i32 signal_read = -1;
    i32 signal_write = -1;
    i32 stop_read = -1;
    i32 stop_write = -1;
#endif

    bool watches(ControlEventKind kind) const noexcept { return (watched & kind_bit(kind)) != 0; }

    void post(ControlEventKind kind) {
        auto queue = delivery;
        relay.send([queue = std::move(queue), kind] { queue->push(kind); });
    }

    void shutdown() noexcept;

    static void destroy(Self *self) noexcept {
        if (self) {
            self->shutdown();
            delete self;
        }
    }
};

namespace {

std::array<std::atomic<ControlEventSource::Self *>, k_max_control_sources> g_sources{};
std::atomic<u32> g_active_handlers{0};
std::mutex g_registry_mutex;

static_assert(std::atomic<ControlEventSource::Self *>::is_always_lock_free);
static_assert(std::atomic<u32>::is_always_lock_free);

void wait_for_handlers() noexcept {
    while (g_active_handlers.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

void publish(ControlEventSource::Self &self) {
    for (usize i = 0; i < g_sources.size(); ++i) {
        ControlEventSource::Self *empty = nullptr;
        if (g_sources[i].compare_exchange_strong(empty, &self, std::memory_order_release, std::memory_order_relaxed)) {
            self.registry_slot = static_cast<isize>(i);
            return;
        }
    }
}

void unpublish(ControlEventSource::Self &self) noexcept {
    if (self.registry_slot < 0) {
        return;
    }
    g_sources[static_cast<usize>(self.registry_slot)].store(nullptr, std::memory_order_release);
    self.registry_slot = -1;
    wait_for_handlers();
}

#ifdef _WIN32

std::atomic<u32> g_windows_source_count{0};

std::optional<ControlEventKind> from_windows_control(DWORD value) noexcept {
    switch (value) {
        case CTRL_C_EVENT: return ControlEventKind::INTERRUPT;
        case CTRL_BREAK_EVENT: return ControlEventKind::BREAK;
        case CTRL_CLOSE_EVENT: return ControlEventKind::HANGUP;
        case CTRL_LOGOFF_EVENT: return ControlEventKind::LOGOFF;
        case CTRL_SHUTDOWN_EVENT: return ControlEventKind::SHUTDOWN;
        default: return std::nullopt;
    }
}

BOOL WINAPI control_handler(DWORD value) {
    g_active_handlers.fetch_add(1, std::memory_order_acquire);

    bool handled = false;
    if (auto kind = from_windows_control(value)) {
        for (auto &slot : g_sources) {
            auto *source = slot.load(std::memory_order_acquire);
            if (!source || !source->watches(*kind)) {
                continue;
            }
            source->pending[kind_index(*kind)].fetch_add(1, std::memory_order_relaxed);
            SetEvent(source->ready);
            handled = true;
        }
    }

    g_active_handlers.fetch_sub(1, std::memory_order_release);
    return handled ? TRUE : FALSE;
}

Error register_native(ControlEventSource::Self &self) {
    std::lock_guard lock(g_registry_mutex);

    publish(self);
    if (self.registry_slot < 0) {
        return Error::k_no_buffer_space_available;
    }

    if (g_windows_source_count.fetch_add(1, std::memory_order_relaxed) == 0 && !SetConsoleCtrlHandler(control_handler, TRUE)) {
        g_windows_source_count.fetch_sub(1, std::memory_order_relaxed);
        unpublish(self);
        return Error::k_io_error;
    }
    return {};
}

void unregister_native(ControlEventSource::Self &self) noexcept {
    std::lock_guard lock(g_registry_mutex);
    if (self.registry_slot < 0) {
        return;
    }

    unpublish(self);
    if (g_windows_source_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        SetConsoleCtrlHandler(control_handler, FALSE);
    }
}

void run_worker(ControlEventSource::Self *self) {
    const HANDLE handles[] = {self->stop, self->ready};
    while (true) {
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
            return;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            continue;
        }

        for (usize i = 0; i < self->pending.size(); ++i) {
            auto count = self->pending[i].exchange(0, std::memory_order_acq_rel);
            while (count-- > 0) {
                self->post(static_cast<ControlEventKind>(i));
            }
        }
    }
}

#else

struct PosixRegistration {
    i32 refs = 0;
    struct sigaction previous{};
};

std::array<PosixRegistration, k_control_kind_count> g_posix_registrations{};

i32 native_signal(ControlEventKind kind) noexcept {
    switch (kind) {
        case ControlEventKind::INTERRUPT: return SIGINT;
        case ControlEventKind::TERMINATE: return SIGTERM;
        case ControlEventKind::HANGUP: return SIGHUP;
        case ControlEventKind::QUIT: return SIGQUIT;
        case ControlEventKind::BREAK:
        case ControlEventKind::LOGOFF:
        case ControlEventKind::SHUTDOWN: return -1;
    }
    return -1;
}

std::optional<ControlEventKind> from_posix_signal(i32 value) noexcept {
    switch (value) {
        case SIGINT: return ControlEventKind::INTERRUPT;
        case SIGTERM: return ControlEventKind::TERMINATE;
        case SIGHUP: return ControlEventKind::HANGUP;
        case SIGQUIT: return ControlEventKind::QUIT;
        default: return std::nullopt;
    }
}

extern "C" void posix_control_handler(i32 value) {
    const i32 saved_errno = errno;
    g_active_handlers.fetch_add(1, std::memory_order_acquire);

    if (auto kind = from_posix_signal(value)) {
        const auto byte = static_cast<u8>(*kind);
        for (auto &slot : g_sources) {
            auto *source = slot.load(std::memory_order_acquire);
            if (!source || !source->watches(*kind)) {
                continue;
            }
            [[maybe_unused]] const auto written = ::write(source->signal_write, &byte, sizeof(byte));
        }
    }

    g_active_handlers.fetch_sub(1, std::memory_order_release);
    errno = saved_errno;
}

Error make_pipe(i32 (&fds)[2]) {
    if (::pipe(fds) != 0) {
        return Error::k_io_error;
    }
    for (auto fd : fds) {
        const auto flags = ::fcntl(fd, F_GETFL, 0);
        const auto descriptor_flags = ::fcntl(fd, F_GETFD, 0);
        if (flags < 0 || descriptor_flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
            ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            fds[0] = -1;
            fds[1] = -1;
            return Error::k_io_error;
        }
    }
    return {};
}

Error register_native(ControlEventSource::Self &self) {
    std::lock_guard lock(g_registry_mutex);

    std::vector<ControlEventKind> installed;
    for (usize i = 0; i < k_control_kind_count; ++i) {
        const auto kind = static_cast<ControlEventKind>(i);
        if (!self.watches(kind)) {
            continue;
        }

        auto &registration = g_posix_registrations[i];
        if (registration.refs++ > 0) {
            installed.push_back(kind);
            continue;
        }

        struct sigaction action{};
        action.sa_handler = posix_control_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (::sigaction(native_signal(kind), &action, &registration.previous) != 0) {
            registration.refs = 0;
            for (auto rollback : installed) {
                auto &old = g_posix_registrations[kind_index(rollback)];
                if (--old.refs == 0) {
                    ::sigaction(native_signal(rollback), &old.previous, nullptr);
                }
            }
            return Error::k_io_error;
        }
        installed.push_back(kind);
    }

    publish(self);
    if (self.registry_slot < 0) {
        for (auto kind : installed) {
            auto &registration = g_posix_registrations[kind_index(kind)];
            if (--registration.refs == 0) {
                ::sigaction(native_signal(kind), &registration.previous, nullptr);
            }
        }
        return Error::k_no_buffer_space_available;
    }
    return {};
}

void unregister_native(ControlEventSource::Self &self) noexcept {
    std::lock_guard lock(g_registry_mutex);
    if (self.registry_slot < 0) {
        return;
    }

    unpublish(self);
    for (usize i = 0; i < k_control_kind_count; ++i) {
        const auto kind = static_cast<ControlEventKind>(i);
        if (!self.watches(kind)) {
            continue;
        }
        auto &registration = g_posix_registrations[i];
        if (--registration.refs == 0) {
            ::sigaction(native_signal(kind), &registration.previous, nullptr);
        }
    }
}

void run_worker(ControlEventSource::Self *self) {
    struct pollfd fds[] = {{self->stop_read, POLLIN, 0}, {self->signal_read, POLLIN, 0}};
    std::array<u8, 128> buffer{};

    while (true) {
        const i32 result = ::poll(fds, 2, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return;
        }
        if ((fds[1].revents & POLLIN) == 0) {
            continue;
        }

        while (true) {
            const auto count = ::read(self->signal_read, buffer.data(), buffer.size());
            if (count <= 0) {
                break;
            }
            for (isize i = 0; i < count; ++i) {
                self->post(static_cast<ControlEventKind>(buffer[static_cast<usize>(i)]));
            }
        }
    }
}

#endif

} // namespace

bool control_event_supported(ControlEventKind kind) noexcept {
#ifdef _WIN32
    // Interactive applications reliably receive Ctrl+C, Ctrl+Break, and
    // console close. Microsoft documents logoff/shutdown delivery for services,
    // not ordinary interactive processes, so do not advertise those here.
    return kind == ControlEventKind::INTERRUPT || kind == ControlEventKind::HANGUP || kind == ControlEventKind::BREAK;
#else
    return kind == ControlEventKind::INTERRUPT || kind == ControlEventKind::TERMINATE || kind == ControlEventKind::HANGUP ||
           kind == ControlEventKind::QUIT;
#endif
}

ControlEventSource::ControlEventSource() noexcept = default;

ControlEventSource::ControlEventSource(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

ControlEventSource::~ControlEventSource() = default;

ControlEventSource::ControlEventSource(ControlEventSource &&other) noexcept = default;

ControlEventSource &ControlEventSource::operator=(ControlEventSource &&other) noexcept = default;

ControlEventSource::Self *ControlEventSource::operator->() noexcept { return self.get(); }

Result<ControlEventSource> ControlEventSource::create(std::span<const ControlEventKind> kinds, EventLoop &loop) {
    if (kinds.empty()) {
        return outcome_error(Error::k_invalid_argument);
    }

    auto self = UniqueHandle<Self>(new Self());
    self->relay = loop.create_relay(Relay::KeepAlive::NO);

    for (auto kind : kinds) {
        if (!control_event_supported(kind) || self->watches(kind)) {
            return outcome_error(Error::k_invalid_argument);
        }
        self->watched |= kind_bit(kind);
    }

#ifdef _WIN32
    self->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    self->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!self->ready || !self->stop) {
        return outcome_error(Error::k_io_error);
    }
#else
    i32 signal_pipe[2] = {-1, -1};
    i32 stop_pipe[2] = {-1, -1};
    if (auto err = make_pipe(signal_pipe)) {
        return outcome_error(err);
    }
    if (auto err = make_pipe(stop_pipe)) {
        ::close(signal_pipe[0]);
        ::close(signal_pipe[1]);
        return outcome_error(err);
    }
    self->signal_read = signal_pipe[0];
    self->signal_write = signal_pipe[1];
    self->stop_read = stop_pipe[0];
    self->stop_write = stop_pipe[1];
#endif

    if (auto err = register_native(*self)) {
        return outcome_error(err);
    }

    self->worker = std::thread(run_worker, self.get());
    return ControlEventSource(std::move(self));
}

Result<ControlEventSource> ControlEventSource::create(std::initializer_list<ControlEventKind> kinds, EventLoop &loop) {
    return create(std::span<const ControlEventKind>(kinds.begin(), kinds.size()), loop);
}

Task<ControlEventKind, Error> ControlEventSource::next() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }
    co_return co_await self->delivery->next(self->relay, true).or_fail();
}

Task<ControlEventKind, Error> ControlEventSource::next_background() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }
    co_return co_await self->delivery->next(self->relay, false).or_fail();
}

void ControlEventSource::hold_loop() noexcept {
    if (self) {
        self->relay.hold();
    }
}

void ControlEventSource::release_loop() noexcept {
    if (self) {
        self->relay.release();
    }
}

bool ControlEventSource::holding_loop() const noexcept { return self && self->relay.holding_loop(); }

void ControlEventSource::Self::shutdown() noexcept {
    unregister_native(*this);

#ifdef _WIN32
    if (stop) {
        SetEvent(stop);
    }
#else
    if (stop_write >= 0) {
        const u8 byte = 1;
        [[maybe_unused]] const auto written = ::write(stop_write, &byte, sizeof(byte));
    }
#endif

    if (worker.joinable()) {
        worker.join();
    }

#ifdef _WIN32
    if (ready) {
        CloseHandle(ready);
        ready = nullptr;
    }
    if (stop) {
        CloseHandle(stop);
        stop = nullptr;
    }
#else
    for (auto *fd : {&signal_read, &signal_write, &stop_read, &stop_write}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
#endif
}

} // namespace lighter
