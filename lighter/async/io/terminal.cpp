#include "terminal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <lighter/async/detail/native_event_queue.h>
#include <lighter/async/detail/terminal_input_decoder.h>
#include <lighter/utils/panic.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace lighter {

struct TerminalSession::Self {
    enum class Lifecycle : u8 {
        EMPTY,
        CAPTURED,
        ACTIVE,
        RUNNING,
    };

    std::shared_ptr<detail::NativeEventQueue<TerminalEvent>> delivery = std::make_shared<detail::NativeEventQueue<TerminalEvent>>();
    Relay relay;
    Options options;
    i32 input_fd = -1;
    i32 output_fd = -1;
    std::thread worker;
    Lifecycle lifecycle = Lifecycle::EMPTY;
    bool owns_process_terminal = false;
    bool virtual_input = true;
    bool alternate_screen_active = false;

#ifdef _WIN32
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    bool owns_input = false;
    bool owns_output = false;
    DWORD original_input_mode = 0;
    DWORD original_output_mode = 0;
    UINT original_input_codepage = 0;
    UINT original_output_codepage = 0;
#else
    termios original_mode{};
    i32 stop_read = -1;
    i32 stop_write = -1;
    i32 resize_read = -1;
    i32 resize_write = -1;
    struct sigaction original_winch{};
    bool winch_installed = false;
#endif

    void post(TerminalEvent event) {
        auto queue = delivery;
        relay.send([queue = std::move(queue), event = std::move(event)]() mutable { queue->push(std::move(event)); });
    }

    Error write_native(std::string_view bytes);
    Error apply_terminal_state(bool enter_alternate_screen = true);
    Error restore_terminal_state(bool leave_alternate_screen = true);
    void arm_emergency_restore(bool restore_modes);
    Error start_worker();
    void stop_worker() noexcept;
    void shutdown() noexcept;

    static void destroy(Self *self) noexcept {
        if (self) {
            self->shutdown();
            delete self;
        }
    }
};

namespace {

std::atomic<TerminalSession::Self *> g_terminal_owner{nullptr};

TerminalEvent key_event(TerminalKey key, TerminalModifiers modifiers = TerminalModifiers::NONE, std::string text = {}, i32 repeat = 1,
                        bool pressed = true) {
    return TerminalEvent{
        .kind = TerminalEventKind::KEY,
        .key = key,
        .modifiers = modifiers,
        .text = std::move(text),
        .repeat = repeat,
        .pressed = pressed,
    };
}

TerminalEvent text_event(std::string text) { return TerminalEvent{.kind = TerminalEventKind::TEXT, .text = std::move(text)}; }

/// Snapshot of what it takes to hand the terminal back to the shell from a
/// panic, contract violation, or std::terminate, where the session object is
/// unreachable. Everything is pre-computed so the hook only performs raw
/// writes and mode resets.
struct EmergencyRestore {
    std::atomic<bool> armed{false};
    bool restore_modes = false;
    /// Hook that was installed before the terminal took over. It runs after
    /// the terminal is restored and is reinstalled when the terminal lets go.
    lighter::PanicHook previous = nullptr;
#ifdef _WIN32
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    UINT input_codepage = 0;
    UINT output_codepage = 0;
#else
    i32 input_fd = -1;
    i32 output_fd = -1;
    termios mode{};
#endif
    std::array<char, 96> sequence{};
    usize sequence_size = 0;
};

EmergencyRestore g_emergency_restore;

void emergency_restore_terminal() noexcept {
    auto &record = g_emergency_restore;
    const auto previous = record.previous;
    if (!record.armed.exchange(false, std::memory_order_acq_rel)) {
        if (previous) {
            previous();
        }
        return;
    }
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(record.output, record.sequence.data(), static_cast<DWORD>(record.sequence_size), &written, nullptr);
    if (record.restore_modes) {
        SetConsoleMode(record.input, record.input_mode);
        SetConsoleMode(record.output, record.output_mode);
        SetConsoleCP(record.input_codepage);
        SetConsoleOutputCP(record.output_codepage);
    }
#else
    std::string_view bytes(record.sequence.data(), record.sequence_size);
    while (!bytes.empty()) {
        const auto written = ::write(record.output_fd, bytes.data(), bytes.size());
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        bytes.remove_prefix(static_cast<usize>(written));
    }
    if (record.restore_modes) {
        ::tcsetattr(record.input_fd, TCSANOW, &record.mode);
    }
#endif
    if (previous) {
        previous();
    }
}

void disarm_emergency_restore() noexcept {
    auto &record = g_emergency_restore;
    record.armed.store(false, std::memory_order_release);
    // Hand the slot back to whoever held it before the terminal. If someone
    // else replaced the terminal's hook in the meantime, theirs stays.
    if (auto current = lighter::set_panic_hook(record.previous); current != &emergency_restore_terminal) {
        lighter::set_panic_hook(current);
    }
    record.previous = nullptr;
}

TerminalEvent resize_event(TerminalSize size) { return TerminalEvent{.kind = TerminalEventKind::RESIZE, .size = size}; }

std::string terminal_features(const TerminalSession::Options &options, bool enable, bool virtual_input, bool alternate_screen) {
    std::string result;
    if (!enable) {
        result += "\x1b[0m\x1b[?25h";
        if (alternate_screen) result += "\x1b[?1049l";
    }
    if (virtual_input) {
        if (enable) {
            result += "\x1b[?2004h";
            if (options.focus_events) {
                result += "\x1b[?1004h";
            }
            if (options.mouse_events) {
                // Button-event tracking (1002) adds motion-while-held reports
                // on top of 1000's press/release/wheel, enabling drag selection.
                result += "\x1b[?1002h\x1b[?1006h";
            }
        } else {
            if (options.mouse_events) {
                result += "\x1b[?1006l\x1b[?1002l";
            }
            if (options.focus_events) {
                result += "\x1b[?1004l";
            }
            result += "\x1b[?2004l";
        }
    }
    if (enable && alternate_screen) {
        result += "\x1b[?1049h";
    }
    return result;
}

#ifdef _WIN32

bool conpty_requested() noexcept {
    const auto *value = std::getenv("LIGHTER_CONPTY");
    return value && std::string_view(value) == "1";
}

HANDLE windows_handle(i32 fd, bool &owned, bool force_console = false) noexcept {
    force_console = force_console || conpty_requested();
    owned = false;
    const auto raw = _get_osfhandle(fd);
    if (raw != -1) {
        auto handle = reinterpret_cast<HANDLE>(raw);
        DWORD mode = 0;
        const auto type = GetFileType(handle);
        if (GetConsoleMode(handle, &mode) || (!force_console && (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK))) {
            return handle;
        }
    }
    HANDLE handle = INVALID_HANDLE_VALUE;
    switch (fd) {
        case 0: handle = GetStdHandle(STD_INPUT_HANDLE); break;
        case 1: handle = GetStdHandle(STD_OUTPUT_HANDLE); break;
        case 2: handle = GetStdHandle(STD_ERROR_HANDLE); break;
        default: return INVALID_HANDLE_VALUE;
    }
    if (handle && handle != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        const auto type = GetFileType(handle);
        if (GetConsoleMode(handle, &mode) || (!force_console && (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK))) {
            return handle;
        }
    }
    // MinGW's CRT descriptors can refer to ConPTY's internal character
    // handles without recognizing them as Console handles. Reopen the
    // process's attached pseudoconsole through its canonical device names.
    const auto name = fd == 0 ? L"CONIN$" : L"CONOUT$";
    handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        owned = true;
    }
    return handle;
}

TerminalModifiers windows_modifiers(DWORD state) noexcept {
    auto result = TerminalModifiers::NONE;
    if ((state & SHIFT_PRESSED) != 0) {
        result = result | TerminalModifiers::SHIFT;
    }
    if ((state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0) {
        result = result | TerminalModifiers::ALT;
    }
    if ((state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0) {
        result = result | TerminalModifiers::CONTROL;
    }
    return result;
}

TerminalKey windows_key(WORD value) noexcept {
    switch (value) {
        case VK_RETURN: return TerminalKey::ENTER;
        case VK_BACK: return TerminalKey::BACKSPACE;
        case VK_TAB: return TerminalKey::TAB;
        case VK_ESCAPE: return TerminalKey::ESCAPE;
        case VK_INSERT: return TerminalKey::INSERT;
        case VK_DELETE: return TerminalKey::DELETE_KEY;
        case VK_HOME: return TerminalKey::HOME;
        case VK_END: return TerminalKey::END;
        case VK_PRIOR: return TerminalKey::PAGE_UP;
        case VK_NEXT: return TerminalKey::PAGE_DOWN;
        case VK_UP: return TerminalKey::ARROW_UP;
        case VK_DOWN: return TerminalKey::ARROW_DOWN;
        case VK_LEFT: return TerminalKey::ARROW_LEFT;
        case VK_RIGHT: return TerminalKey::ARROW_RIGHT;
        case VK_F1: return TerminalKey::F1;
        case VK_F2: return TerminalKey::F2;
        case VK_F3: return TerminalKey::F3;
        case VK_F4: return TerminalKey::F4;
        case VK_F5: return TerminalKey::F5;
        case VK_F6: return TerminalKey::F6;
        case VK_F7: return TerminalKey::F7;
        case VK_F8: return TerminalKey::F8;
        case VK_F9: return TerminalKey::F9;
        case VK_F10: return TerminalKey::F10;
        case VK_F11: return TerminalKey::F11;
        case VK_F12: return TerminalKey::F12;
        default: return TerminalKey::CHARACTER;
    }
}

std::string utf16_to_utf8(const WCHAR *text, i32 size) {
    if (size <= 0) {
        return {};
    }
    const i32 required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<usize>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, size, result.data(), required, nullptr, nullptr);
    return result;
}

TerminalSize windows_size(HANDLE output) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(output, &info)) {
        return {};
    }
    return {
        .columns = static_cast<i32>(info.srWindow.Right - info.srWindow.Left + 1),
        .rows = static_cast<i32>(info.srWindow.Bottom - info.srWindow.Top + 1),
    };
}

void run_terminal_worker(TerminalSession::Self *self) {
    const HANDLE handles[] = {self->stop, self->input};
    if (self->virtual_input) {
        std::array<char, 4096> buffer{};
        detail::TerminalInputDecoder decoder;
        auto last_size = windows_size(self->output);
        auto emit = [self](TerminalEvent event) { self->post(std::move(event)); };
        while (true) {
            const DWORD timeout = decoder.escape_pending() ? 25 : INFINITE;
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, timeout);
            if (wait == WAIT_OBJECT_0) {
                return;
            }
            if (wait == WAIT_TIMEOUT) {
                decoder.flush_escape(emit);
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                return;
            }

            // ConPTY normally exposes WINDOW_BUFFER_SIZE_EVENT records, but a
            // resize racing readable VT input can coalesce the record. Polling
            // the canonical output size whenever input wakes the worker keeps
            // resize delivery deterministic without a timer or busy loop.
            const auto current_size = windows_size(self->output);
            if (current_size.columns > 0 && current_size.rows > 0 && current_size != last_size) {
                last_size = current_size;
                self->post(resize_event(current_size));
            }

            INPUT_RECORD record{};
            DWORD available = 0;
            if (!PeekConsoleInputW(self->input, &record, 1, &available)) {
                self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                return;
            }
            if (available != 0 && record.EventType != KEY_EVENT) {
                DWORD consumed = 0;
                if (!ReadConsoleInputW(self->input, &record, 1, &consumed)) {
                    self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                    return;
                }
                if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                    const auto size = windows_size(self->output);
                    if (size.columns > 0 && size.rows > 0 && size != last_size) {
                        last_size = size;
                        self->post(resize_event(size));
                    }
                }
                continue;
            }

            DWORD count = 0;
            if (!ReadFile(self->input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                return;
            }
            if (count == 0) {
                self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                return;
            }
            std::string_view bytes(buffer.data(), count);
            auto interrupt = bytes.find('\x03');
            while (interrupt != std::string_view::npos) {
                decoder.feed(bytes.substr(0, interrupt), emit);
                if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
                    self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                    return;
                }
                bytes.remove_prefix(interrupt + 1);
                interrupt = bytes.find('\x03');
            }
            decoder.feed(bytes, emit);
        }
    }

    std::array<INPUT_RECORD, 64> records{};
    WCHAR high_surrogate = 0;

    while (true) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            return;
        }
        if (wait == WAIT_FAILED) {
            self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
            return;
        }
        if (wait != WAIT_OBJECT_0 + 1) {
            continue;
        }

        DWORD count = 0;
        if (!ReadConsoleInputW(self->input, records.data(), static_cast<DWORD>(records.size()), &count)) {
            self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
            return;
        }

        for (DWORD i = 0; i < count; ++i) {
            const auto &record = records[i];
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                self->post(resize_event(windows_size(self->output)));
                continue;
            }
            if (record.EventType == FOCUS_EVENT && self->options.focus_events) {
                self->post(TerminalEvent{.kind = TerminalEventKind::FOCUS, .focused = record.Event.FocusEvent.bSetFocus != FALSE});
                continue;
            }
            if (record.EventType == MOUSE_EVENT && self->options.mouse_events) {
                const auto &mouse = record.Event.MouseEvent;
                const auto wheel = (mouse.dwEventFlags & MOUSE_WHEELED) != 0 ? static_cast<i16>(HIWORD(mouse.dwButtonState)) : 0;
                self->post(TerminalEvent{
                    .kind = TerminalEventKind::MOUSE,
                    .modifiers = windows_modifiers(mouse.dwControlKeyState),
                    .x = mouse.dwMousePosition.X,
                    .y = mouse.dwMousePosition.Y,
                    .mouse_buttons = static_cast<i32>(mouse.dwButtonState & 0xffff),
                    .wheel_delta = wheel,
                });
                continue;
            }
            if (record.EventType != KEY_EVENT) {
                continue;
            }

            const auto &key = record.Event.KeyEvent;
            std::string text;
            const WCHAR character = key.uChar.UnicodeChar;
            if (character >= 0xd800 && character <= 0xdbff) {
                high_surrogate = character;
            } else if (character >= 0xdc00 && character <= 0xdfff && high_surrogate != 0) {
                const WCHAR pair[] = {high_surrogate, character};
                text = utf16_to_utf8(pair, 2);
                high_surrogate = 0;
            } else {
                high_surrogate = 0;
                if (character != 0 && (character >= 0x20 || character == L'\t')) {
                    text = utf16_to_utf8(&character, 1);
                }
            }

            const auto mapped = windows_key(key.wVirtualKeyCode);
            const auto modifiers = windows_modifiers(key.dwControlKeyState);
            if (mapped == TerminalKey::CHARACTER && text.empty() && has_modifier(modifiers, TerminalModifiers::CONTROL) &&
                key.wVirtualKeyCode >= 'A' && key.wVirtualKeyCode <= 'Z') {
                text = std::string(1, static_cast<char>('a' + key.wVirtualKeyCode - 'A'));
            }
            if (mapped == TerminalKey::CHARACTER && !text.empty() && modifiers == TerminalModifiers::NONE && key.bKeyDown) {
                if (key.wRepeatCount > 1) {
                    const auto unit = text;
                    for (WORD repeat = 1; repeat < key.wRepeatCount; ++repeat) {
                        text += unit;
                    }
                }
                self->post(text_event(std::move(text)));
            } else {
                self->post(key_event(mapped, modifiers, std::move(text), key.wRepeatCount, key.bKeyDown != FALSE));
            }
        }
    }
}

#else

std::atomic<u32> g_winch_handlers{0};
static_assert(std::atomic<TerminalSession::Self *>::is_always_lock_free);
static_assert(std::atomic<u32>::is_always_lock_free);

extern "C" void terminal_winch_handler(i32) {
    const i32 saved_errno = errno;
    g_winch_handlers.fetch_add(1, std::memory_order_acquire);
    if (auto *owner = g_terminal_owner.load(std::memory_order_acquire)) {
        const u8 byte = 1;
        [[maybe_unused]] const auto written = ::write(owner->resize_write, &byte, sizeof(byte));
    }
    g_winch_handlers.fetch_sub(1, std::memory_order_release);
    errno = saved_errno;
}

Error make_pipe(i32 &read_fd, i32 &write_fd) {
    i32 fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return Error::k_io_error;
    }
    for (auto fd : fds) {
        const auto status = ::fcntl(fd, F_GETFL, 0);
        const auto descriptor = ::fcntl(fd, F_GETFD, 0);
        if (status < 0 || descriptor < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) != 0 ||
            ::fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return Error::k_io_error;
        }
    }
    read_fd = fds[0];
    write_fd = fds[1];
    return {};
}

TerminalSize posix_size(i32 fd) {
    struct winsize size{};
    if (::ioctl(fd, TIOCGWINSZ, &size) != 0) {
        return {};
    }
    return {.columns = size.ws_col, .rows = size.ws_row};
}

void run_terminal_worker(TerminalSession::Self *self) {
    struct pollfd fds[] = {
        {self->stop_read, POLLIN, 0},
        {self->resize_read, POLLIN, 0},
        {self->input_fd, POLLIN, 0},
    };
    std::array<char, 4096> buffer{};
    detail::TerminalInputDecoder decoder;
    auto emit = [self](TerminalEvent event) { self->post(std::move(event)); };

    while (true) {
        const i32 timeout = decoder.escape_pending() ? 25 : -1;
        const i32 result = ::poll(fds, 3, timeout);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
            return;
        }
        if (result == 0) {
            decoder.flush_escape(emit);
            continue;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            while (::read(self->resize_read, buffer.data(), buffer.size()) > 0) {}
            self->post(resize_event(posix_size(self->output_fd)));
        }
        if ((fds[2].revents & (POLLHUP | POLLERR)) != 0) {
            self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
            return;
        }
        if ((fds[2].revents & POLLIN) != 0) {
            const auto count = ::read(self->input_fd, buffer.data(), buffer.size());
            if (count <= 0) {
                if (count == 0 || errno != EAGAIN) {
                    self->post(TerminalEvent{.kind = TerminalEventKind::CLOSED});
                    return;
                }
            } else {
                decoder.feed(std::string_view(buffer.data(), static_cast<usize>(count)), emit);
            }
        }
    }
}

#endif

} // namespace

TerminalSession::TerminalSession() noexcept = default;

TerminalSession::TerminalSession(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

TerminalSession::~TerminalSession() = default;

TerminalSession::TerminalSession(TerminalSession &&other) noexcept = default;

TerminalSession &TerminalSession::operator=(TerminalSession &&other) noexcept = default;

TerminalSession::Self *TerminalSession::operator->() noexcept { return self.get(); }

bool TerminalSession::attached(i32 fd) noexcept {
#ifdef _WIN32
    bool owned = false;
    const auto handle = windows_handle(fd, owned, conpty_requested());
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    const bool result = GetConsoleMode(handle, &mode) != FALSE;
    if (owned) CloseHandle(handle);
    return result;
#else
    return ::isatty(fd) == 1;
#endif
}

Result<TerminalSession> TerminalSession::open(i32 input_fd, i32 output_fd, Options options, EventLoop &loop) {
#ifndef _WIN32
    if (!attached(input_fd) || !attached(output_fd)) {
        return outcome_error(Error::k_inappropriate_ioctl_for_device);
    }
#endif

    auto self = UniqueHandle<Self>(new Self());
    self->relay = loop.create_relay();
    self->options = options;
    self->input_fd = input_fd;
    self->output_fd = output_fd;

    Self *expected = nullptr;
    if (!g_terminal_owner.compare_exchange_strong(expected, self.get(), std::memory_order_acq_rel)) {
        return outcome_error(Error::k_resource_busy_or_locked);
    }
    self->owns_process_terminal = true;

#ifdef _WIN32
    self->input = windows_handle(input_fd, self->owns_input);
    self->virtual_input = self->owns_input;
    self->output = windows_handle(output_fd, self->owns_output, self->virtual_input);
    if (!GetConsoleMode(self->input, &self->original_input_mode) || !GetConsoleMode(self->output, &self->original_output_mode)) {
        return outcome_error(Error::k_inappropriate_ioctl_for_device);
    }
    self->lifecycle = Self::Lifecycle::CAPTURED;
    self->original_input_codepage = GetConsoleCP();
    self->original_output_codepage = GetConsoleOutputCP();
    self->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!self->stop) {
        return outcome_error(Error::k_io_error);
    }
#else
    if (::tcgetattr(input_fd, &self->original_mode) != 0) {
        return outcome_error(Error::k_io_error);
    }
    self->lifecycle = Self::Lifecycle::CAPTURED;
    if (auto err = make_pipe(self->stop_read, self->stop_write)) {
        return outcome_error(err);
    }
    if (auto err = make_pipe(self->resize_read, self->resize_write)) {
        return outcome_error(err);
    }

    struct sigaction action{};
    action.sa_handler = terminal_winch_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGWINCH, &action, &self->original_winch) != 0) {
        return outcome_error(Error::k_io_error);
    }
    self->winch_installed = true;
#endif

    if (auto err = self->apply_terminal_state()) {
        return outcome_error(err);
    }
    if (auto err = self->start_worker()) {
        self->restore_terminal_state();
        return outcome_error(err);
    }
    return TerminalSession(std::move(self));
}

Task<TerminalEvent, Error> TerminalSession::next_event() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }
    co_return co_await self->delivery->next(self->relay, true).or_fail();
}

Result<TerminalSize> TerminalSession::size() const {
    if (!self) {
        return outcome_error(Error::k_invalid_argument);
    }
#ifdef _WIN32
    auto value = windows_size(self->output);
#else
    auto value = posix_size(self->output_fd);
#endif
    if (value.columns <= 0 || value.rows <= 0) {
        return outcome_error(Error::k_io_error);
    }
    return value;
}

Error TerminalSession::write(std::string_view bytes) {
    if (!self || self->lifecycle != Self::Lifecycle::RUNNING) {
        return Error::k_invalid_argument;
    }
    return self->write_native(bytes);
}

Error TerminalSession::suspend() {
    if (!self || self->lifecycle != Self::Lifecycle::RUNNING) {
        return Error::k_invalid_argument;
    }
    self->stop_worker();
    return self->restore_terminal_state();
}

Error TerminalSession::resume() {
    if (!self || self->lifecycle != Self::Lifecycle::CAPTURED) {
        return Error::k_invalid_argument;
    }
    if (auto err = self->apply_terminal_state()) {
        return err;
    }
    if (auto err = self->start_worker()) {
        self->restore_terminal_state();
        return err;
    }
    return {};
}

Error TerminalSession::handoff() {
    if (!self || self->lifecycle != Self::Lifecycle::RUNNING) {
        return Error::k_invalid_argument;
    }
    self->stop_worker();
    return self->restore_terminal_state(false);
}

Error TerminalSession::reclaim() {
    if (!self || self->lifecycle != Self::Lifecycle::CAPTURED) {
        return Error::k_invalid_argument;
    }
    if (auto err = self->apply_terminal_state(false)) {
        return err;
    }
    if (auto err = self->start_worker()) {
        self->restore_terminal_state(false);
        return err;
    }
    return {};
}

bool TerminalSession::active() const noexcept { return self && self->lifecycle == Self::Lifecycle::RUNNING; }

Error TerminalSession::Self::write_native(std::string_view bytes) {
#ifdef _WIN32
    while (!bytes.empty()) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(std::min<usize>(bytes.size(), 0x7fffffff));
        if (!WriteFile(output, bytes.data(), requested, &written, nullptr)) {
            return Error::k_io_error;
        }
        if (written == 0) {
            return Error::k_io_error;
        }
        bytes.remove_prefix(written);
    }
#else
    while (!bytes.empty()) {
        const auto written = ::write(output_fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error::k_io_error;
        }
        if (written == 0) {
            return Error::k_io_error;
        }
        bytes.remove_prefix(static_cast<usize>(written));
    }
#endif
    return {};
}

Error TerminalSession::Self::apply_terminal_state(bool enter_alternate_screen) {
    if (lifecycle != Lifecycle::CAPTURED) {
        return Error::k_invalid_argument;
    }
#ifdef _WIN32
    DWORD input_mode = original_input_mode;
    input_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_QUICK_EDIT_MODE);
    input_mode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    if (virtual_input) {
        // ConPTY transports Ctrl+C as byte 0x03. Read it in the VT worker and
        // turn it back into a console control so the process-wide interrupt
        // source observes the same event as the native Console backend.
        input_mode &= ~ENABLE_PROCESSED_INPUT;
        input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    } else {
        // Keep processed input so Ctrl+C remains a process-control event even
        // while a provider turn, rather than input reading, owns the loop.
        input_mode |= ENABLE_PROCESSED_INPUT;
    }
    if (options.mouse_events) {
        input_mode |= ENABLE_MOUSE_INPUT;
    } else {
        input_mode &= ~ENABLE_MOUSE_INPUT;
    }
    const DWORD output_mode = original_output_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(input, input_mode)) {
        return Error::k_io_error;
    }
    if (!SetConsoleMode(output, output_mode)) {
        SetConsoleMode(input, original_input_mode);
        return Error::k_io_error;
    }
    if (!SetConsoleCP(CP_UTF8) || !SetConsoleOutputCP(CP_UTF8)) {
        SetConsoleCP(original_input_codepage);
        SetConsoleOutputCP(original_output_codepage);
        SetConsoleMode(input, original_input_mode);
        SetConsoleMode(output, original_output_mode);
        return Error::k_io_error;
    }
#else
    auto raw = original_mode;
    cfmakeraw(&raw);
    // Keep ISIG for the same reason as ENABLE_PROCESSED_INPUT on Windows:
    // Ctrl+C must interrupt work even when nobody is awaiting terminal input.
    raw.c_lflag |= ISIG;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(input_fd, TCSAFLUSH, &raw) != 0) {
        return Error::k_io_error;
    }
#endif
    lifecycle = Lifecycle::ACTIVE;
    alternate_screen_active = alternate_screen_active || enter_alternate_screen;
    arm_emergency_restore(true);
    return write_native(terminal_features(options, true, virtual_input, enter_alternate_screen));
}

/// Arms the panic hook with the sequence and modes that return the terminal to
/// the shell from the current state. With `restore_modes` false only the
/// alternate screen is left, for the editor handoff where the modes are
/// already the shell's but the screen stays resident.
void TerminalSession::Self::arm_emergency_restore(bool restore_modes) {
    auto &record = g_emergency_restore;
    record.armed.store(false, std::memory_order_release);
    record.restore_modes = restore_modes;
#ifdef _WIN32
    record.input = input;
    record.output = output;
    record.input_mode = original_input_mode;
    record.output_mode = original_output_mode;
    record.input_codepage = original_input_codepage;
    record.output_codepage = original_output_codepage;
#else
    record.input_fd = input_fd;
    record.output_fd = output_fd;
    record.mode = original_mode;
#endif
    const auto sequence = terminal_features(options, false, restore_modes && virtual_input, alternate_screen_active);
    record.sequence_size = std::min(sequence.size(), record.sequence.size());
    std::copy_n(sequence.data(), record.sequence_size, record.sequence.data());
    // Re-arming (resume, reclaim) finds our own hook installed; keep chaining
    // to the hook that preceded the terminal rather than to ourselves.
    if (auto prior = lighter::set_panic_hook(&emergency_restore_terminal); prior != &emergency_restore_terminal) {
        record.previous = prior;
    }
    record.armed.store(true, std::memory_order_release);
}

Error TerminalSession::Self::restore_terminal_state(bool leave_alternate_screen) {
    if (lifecycle == Lifecycle::EMPTY || lifecycle == Lifecycle::CAPTURED) {
        return {};
    }
    if (lifecycle != Lifecycle::ACTIVE) {
        return Error::k_invalid_argument;
    }
    const auto sequence_error = write_native(terminal_features(options, false, virtual_input, leave_alternate_screen));
#ifdef _WIN32
    const bool modes_ok = SetConsoleMode(input, original_input_mode) && SetConsoleMode(output, original_output_mode);
    const bool codepages_ok = SetConsoleCP(original_input_codepage) && SetConsoleOutputCP(original_output_codepage);
    if (!modes_ok || !codepages_ok) {
        return Error::k_io_error;
    }
#else
    if (::tcsetattr(input_fd, TCSAFLUSH, &original_mode) != 0) {
        return Error::k_io_error;
    }
#endif
    lifecycle = Lifecycle::CAPTURED;
    if (leave_alternate_screen) {
        alternate_screen_active = false;
    }
    if (alternate_screen_active) {
        arm_emergency_restore(false);
    } else {
        disarm_emergency_restore();
    }
    return sequence_error;
}

Error TerminalSession::Self::start_worker() {
    if (lifecycle != Lifecycle::ACTIVE) {
        return Error::k_invalid_argument;
    }
#ifdef _WIN32
    ResetEvent(stop);
#else
    std::array<char, 64> buffer{};
    while (::read(stop_read, buffer.data(), buffer.size()) > 0) {}
#endif
    try {
        worker = std::thread(run_terminal_worker, this);
    } catch (...) {
        return Error::k_resource_temporarily_unavailable;
    }
    lifecycle = Lifecycle::RUNNING;
    return {};
}

void TerminalSession::Self::stop_worker() noexcept {
    if (lifecycle != Lifecycle::RUNNING) {
        return;
    }
#ifdef _WIN32
    SetEvent(stop);
#else
    const u8 byte = 1;
    [[maybe_unused]] const auto written = ::write(stop_write, &byte, sizeof(byte));
#endif
    if (worker.joinable()) {
        worker.join();
    }
    lifecycle = Lifecycle::ACTIVE;
}

void TerminalSession::Self::shutdown() noexcept {
    stop_worker();
    restore_terminal_state();

    if (owns_process_terminal) {
        g_terminal_owner.store(nullptr, std::memory_order_release);
        owns_process_terminal = false;
    }

#ifdef _WIN32
    if (stop) {
        CloseHandle(stop);
        stop = nullptr;
    }
    if (owns_input) {
        CloseHandle(input);
        input = INVALID_HANDLE_VALUE;
        owns_input = false;
    }
    if (owns_output) {
        CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
        owns_output = false;
    }
#else
    while (g_winch_handlers.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    if (winch_installed) {
        ::sigaction(SIGWINCH, &original_winch, nullptr);
        winch_installed = false;
    }
    for (auto *fd : {&stop_read, &stop_write, &resize_read, &resize_write}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
#endif
}

} // namespace lighter
