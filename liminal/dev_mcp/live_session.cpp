#include "live_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <lighter/encoding/utf8.h>

#include "liminal/tui/surface.h"

namespace liminal::dev_mcp {

namespace {

constexpr usize k_max_output_bytes = 4 * 1024 * 1024;

struct TerminalMirror {
    explicit TerminalMirror(i32 columns, i32 rows) : surface(columns, rows) {}

    void resize(i32 columns, i32 rows) {
        surface = tui::Surface(columns, rows);
        cursor = {};
        pending.clear();
    }

    void feed(std::string_view bytes) {
        pending.append(bytes);
        usize offset = 0;
        while (offset < pending.size()) {
            const auto byte = static_cast<unsigned char>(pending[offset]);
            if (byte == 0x1b) {
                if (offset + 1 >= pending.size()) break;
                if (pending[offset + 1] == '[') {
                    auto end = offset + 2;
                    while (end < pending.size()) {
                        const auto final = static_cast<unsigned char>(pending[end]);
                        if (final >= 0x40 && final <= 0x7e) break;
                        ++end;
                    }
                    if (end == pending.size()) break;
                    apply_csi(std::string_view(pending).substr(offset + 2, end - offset - 2), pending[end]);
                    offset = end + 1;
                    continue;
                }
                if (pending[offset + 1] == ']') {
                    auto end = offset + 2;
                    while (end < pending.size() && pending[end] != '\a' &&
                           !(pending[end] == '\x1b' && end + 1 < pending.size() && pending[end + 1] == '\\')) {
                        ++end;
                    }
                    if (end == pending.size()) break;
                    offset = pending[end] == '\a' ? end + 1 : end + 2;
                    continue;
                }
                offset += 2;
                continue;
            }
            if (byte == '\r') {
                cursor.column = 0;
                ++offset;
                continue;
            }
            if (byte == '\n') {
                cursor.row = std::min(cursor.row + 1, std::max(surface.rows - 1, 0));
                ++offset;
                continue;
            }
            if (byte == '\b') {
                cursor.column = std::max(cursor.column - 1, 0);
                ++offset;
                continue;
            }
            if (byte < 0x20 || byte == 0x7f) {
                ++offset;
                continue;
            }

            auto end = offset;
            while (end < pending.size()) {
                const auto candidate = static_cast<unsigned char>(pending[end]);
                if (candidate == 0x1b || candidate < 0x20 || candidate == 0x7f) break;
                ++end;
            }
            auto text = std::string_view(pending).substr(offset, end - offset);
            const auto complete = lighter::encoding::utf8::complete_prefix_len(text);
            if (complete == 0) break;
            cursor.column = surface.write(cursor.row, cursor.column, text.substr(0, complete));
            offset += complete;
            if (complete < text.size()) break;
        }
        pending.erase(0, offset);
    }

    std::vector<std::string> visible_text() const {
        std::vector<std::string> lines;
        lines.reserve(static_cast<usize>(surface.rows));
        for (i32 row = 0; row < surface.rows; ++row) lines.push_back(surface.row_text(row));
        return lines;
    }

    void apply_csi(std::string_view parameters, char command) {
        const auto values = parse_parameters(parameters);
        const auto value = [&values](usize index, i32 fallback) {
            return index < values.size() && values[index] != 0 ? values[index] : fallback;
        };
        switch (command) {
            case 'H':
            case 'f':
                cursor.row = std::clamp(value(0, 1) - 1, 0, std::max(surface.rows - 1, 0));
                cursor.column = std::clamp(value(1, 1) - 1, 0, std::max(surface.columns - 1, 0));
                break;
            case 'A': cursor.row = std::max(cursor.row - value(0, 1), 0); break;
            case 'B': cursor.row = std::min(cursor.row + value(0, 1), std::max(surface.rows - 1, 0)); break;
            case 'C': cursor.column = std::min(cursor.column + value(0, 1), std::max(surface.columns - 1, 0)); break;
            case 'D': cursor.column = std::max(cursor.column - value(0, 1), 0); break;
            case 'G': cursor.column = std::clamp(value(0, 1) - 1, 0, std::max(surface.columns - 1, 0)); break;
            case 'J':
                if (value(0, 0) == 2 || value(0, 0) == 3) surface.clear();
                break;
            case 'K': clear_row(); break;
            case 'h':
                if (parameters == "?25") cursor.visible = true;
                if (parameters == "?1049") surface.clear();
                break;
            case 'l':
                if (parameters == "?25") cursor.visible = false;
                break;
            default: break;
        }
    }

    static std::vector<i32> parse_parameters(std::string_view parameters) {
        if (!parameters.empty() && (parameters.front() == '?' || parameters.front() == '>')) parameters.remove_prefix(1);
        std::vector<i32> values;
        i32 current = 0;
        bool present = false;
        for (const auto character : parameters) {
            if (character >= '0' && character <= '9') {
                present = true;
                current = std::min(current * 10 + (character - '0'), 10000);
            } else if (character == ';') {
                values.push_back(present ? current : 0);
                current = 0;
                present = false;
            }
        }
        if (present || !parameters.empty()) values.push_back(present ? current : 0);
        return values;
    }

    void clear_row() {
        if (cursor.row < 0 || cursor.row >= surface.rows) return;
        const auto begin = surface.cells.begin() + static_cast<isize>(cursor.row * surface.columns);
        std::fill(begin, begin + surface.columns, tui::Cell{});
    }

    tui::Surface surface;
    tui::Cursor cursor;
    std::string pending;
};

std::expected<std::filesystem::path, std::string> resolve_working_directory(std::string_view requested) {
    std::error_code error;
    auto path = std::filesystem::canonical(std::filesystem::path(requested), error);
    if (error) return std::unexpected("cannot resolve working directory: " + error.message());
    if (!std::filesystem::is_directory(path, error) || error) return std::unexpected("working directory is not a directory");
    return path;
}

#ifdef _WIN32

std::string windows_error(std::string_view operation) {
    return std::string(operation) + " failed: " + std::system_category().message(static_cast<int>(GetLastError()));
}

std::expected<std::filesystem::path, std::string> executable_path() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) return std::unexpected(windows_error("GetModuleFileNameW"));
    buffer.resize(length);
    auto path = std::filesystem::path(buffer).parent_path() / L"liminal.exe";
    if (!std::filesystem::is_regular_file(path)) return std::unexpected("cannot find sibling liminal.exe");
    return path;
}

std::expected<std::filesystem::path, std::string> helper_path() {
    auto executable = executable_path();
    if (!executable) return std::unexpected(executable.error());
    auto path = executable->parent_path() / L"windows_pty.py";
    if (!std::filesystem::is_regular_file(path)) return std::unexpected("cannot find sibling windows_pty.py");
    return path;
}

std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<usize>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

#else

std::expected<std::filesystem::path, std::string> executable_path() {
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length < 0) return std::unexpected(std::string("cannot resolve MCP executable: ") + std::strerror(errno));
    auto path = std::filesystem::path(std::string_view(buffer.data(), static_cast<usize>(length))).parent_path() / "liminal";
    if (!std::filesystem::is_regular_file(path)) return std::unexpected("cannot find sibling liminal");
    return path;
}

#endif

std::optional<std::string_view> key_bytes(std::string_view name) {
    if (name == "enter") return "\r";
    if (name == "escape") return "\x1b";
    if (name == "backspace") return "\x7f";
    if (name == "ctrl_c") return "\x03";
    if (name == "ctrl_j") return "\x0a";
    if (name == "tab") return "\t";
    if (name == "up") return "\x1b[A";
    if (name == "down") return "\x1b[B";
    if (name == "right") return "\x1b[C";
    if (name == "left") return "\x1b[D";
    if (name == "page_up") return "\x1b[5~";
    if (name == "page_down") return "\x1b[6~";
    if (name == "home") return "\x1b[H";
    if (name == "end") return "\x1b[F";
    if (name == "delete") return "\x1b[3~";
    return std::nullopt;
}

std::string escape_control_bytes(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(bytes.size());
    for (const auto value : bytes) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20 || byte == 0x7f) {
            escaped += "\\x";
            escaped += digits[byte >> 4];
            escaped += digits[byte & 0x0f];
        } else {
            escaped += value;
        }
    }
    return escaped;
}

std::string hex_encode(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = static_cast<unsigned char>(value);
        encoded += digits[byte >> 4];
        encoded += digits[byte & 0x0f];
    }
    return encoded;
}

std::expected<std::string, std::string> hex_decode(std::string_view encoded) {
    if (encoded == "-") return std::string{};
    if (encoded.size() % 2 != 0) return std::unexpected("invalid helper hex response");
    const auto digit = [](char value) -> i32 {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (usize offset = 0; offset < encoded.size(); offset += 2) {
        const auto high = digit(encoded[offset]);
        const auto low = digit(encoded[offset + 1]);
        if (high < 0 || low < 0) return std::unexpected("invalid helper hex response");
        decoded += static_cast<char>((high << 4) | low);
    }
    return decoded;
}

} // namespace

struct LiveSession::Impl {
    explicit Impl(i32 columns, i32 rows) : columns(columns), rows(rows), mirror(columns, rows) {}

    void append_output(std::string_view bytes) {
        mirror.feed(bytes);
        output.append(bytes);
        if (output.size() > k_max_output_bytes) {
            const auto removed = output.size() - k_max_output_bytes;
            output.erase(0, removed);
            output_offset += removed;
        }
    }

    std::expected<void, std::string> drain();
    std::expected<void, std::string> refresh_process();
    void close() noexcept;
#ifdef _WIN32
    std::expected<std::string, std::string> helper_command(std::string command);
#endif

    i32 columns;
    i32 rows;
    TerminalMirror mirror;
    std::string output;
    u64 output_offset = 0;
    bool running = true;
    std::optional<i32> exit_code;
    u64 process_id = 0;

#ifdef _WIN32
    HANDLE input = nullptr;
    HANDLE output_pipe = nullptr;
    HANDLE helper_process = nullptr;
#else
    int master = -1;
    pid_t process = -1;
#endif
};

#ifdef _WIN32

std::expected<std::string, std::string> LiveSession::Impl::helper_command(std::string command) {
    command += '\n';
    usize offset = 0;
    while (offset < command.size()) {
        DWORD written = 0;
        if (!WriteFile(input, command.data() + offset, static_cast<DWORD>(command.size() - offset), &written, nullptr)) {
            return std::unexpected(windows_error("writing PTY helper command"));
        }
        if (written == 0) return std::unexpected("writing PTY helper command made no progress");
        offset += written;
    }

    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() <= 16 * 1024 * 1024) {
        DWORD read = 0;
        if (!ReadFile(output_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            return std::unexpected(windows_error("reading PTY helper response"));
        }
        response.append(buffer.data(), read);
        if (const auto newline = response.find('\n'); newline != std::string::npos) {
            response.resize(newline);
            break;
        }
    }
    if (response.size() > 16 * 1024 * 1024) return std::unexpected("PTY helper response limit exceeded");
    if (response == "OK") return std::string{};
    if (response.starts_with("OK ")) return response.substr(3);
    if (response.starts_with("ERR ")) {
        auto message = hex_decode(std::string_view(response).substr(4));
        return std::unexpected(message ? *std::move(message) : std::string("PTY helper reported an error"));
    }
    return std::unexpected("invalid PTY helper response: " + escape_control_bytes(response.substr(0, 256)));
}

std::expected<void, std::string> LiveSession::Impl::drain() {
    auto response = helper_command("READ");
    if (!response) return std::unexpected(response.error());
    std::istringstream fields(*response);
    i32 is_running = 0;
    i32 code = -1;
    std::string encoded;
    if (!(fields >> is_running >> code >> encoded)) return std::unexpected("invalid PTY helper READ response");
    running = is_running != 0;
    if (!running) exit_code = code;
    auto bytes = hex_decode(encoded);
    if (!bytes) return std::unexpected(bytes.error());
    append_output(*bytes);
    return {};
}

std::expected<void, std::string> LiveSession::Impl::refresh_process() { return drain(); }

void LiveSession::Impl::close() noexcept {
    if (input && output_pipe) (void) helper_command("CLOSE");
    if (input) CloseHandle(std::exchange(input, nullptr));
    if (output_pipe) CloseHandle(std::exchange(output_pipe, nullptr));
    if (helper_process) {
        if (WaitForSingleObject(helper_process, 2000) == WAIT_TIMEOUT) TerminateProcess(helper_process, 1);
        CloseHandle(std::exchange(helper_process, nullptr));
    }
}

#else

std::expected<void, std::string> LiveSession::Impl::drain() {
    std::array<char, 4096> buffer{};
    while (master >= 0) {
        const auto count = ::read(master, buffer.data(), buffer.size());
        if (count > 0) {
            append_output(std::string_view(buffer.data(), static_cast<usize>(count)));
            continue;
        }
        if (count == 0 || errno == EIO) return {};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
        return std::unexpected(std::string("reading PTY failed: ") + std::strerror(errno));
    }
    return {};
}

std::expected<void, std::string> LiveSession::Impl::refresh_process() {
    if (!running) return {};
    int status = 0;
    const auto result = waitpid(process, &status, WNOHANG);
    if (result == 0) return {};
    if (result < 0) return std::unexpected(std::string("waitpid failed: ") + std::strerror(errno));
    running = false;
    if (WIFEXITED(status))
        exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_code = 128 + WTERMSIG(status);
    return {};
}

void LiveSession::Impl::close() noexcept {
    if (running && process > 0) {
        kill(-process, SIGTERM);
        for (i32 attempt = 0; attempt < 20; ++attempt) {
            int status = 0;
            if (waitpid(process, &status, WNOHANG) == process) {
                running = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (running) {
            kill(-process, SIGKILL);
            waitpid(process, nullptr, 0);
            running = false;
        }
    }
    if (master >= 0) ::close(std::exchange(master, -1));
}

#endif

LiveSession::LiveSession(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

LiveSession::~LiveSession() { impl->close(); }

std::expected<std::unique_ptr<LiveSession>, std::string> LiveSession::create(std::string_view working_directory, i32 columns, i32 rows) {
    auto directory = resolve_working_directory(working_directory);
    if (!directory) return std::unexpected(directory.error());
    auto executable = executable_path();
    if (!executable) return std::unexpected(executable.error());
    auto state = std::make_unique<Impl>(columns, rows);

#ifdef _WIN32
    auto helper = helper_path();
    if (!helper) return std::unexpected(helper.error());
    SECURITY_ATTRIBUTES security{.nLength = sizeof(security), .lpSecurityDescriptor = nullptr, .bInheritHandle = true};
    HANDLE helper_input = nullptr;
    HANDLE host_input = nullptr;
    HANDLE host_output = nullptr;
    HANDLE helper_output = nullptr;
    HANDLE null_error = nullptr;
    const auto cleanup = [&] {
        if (helper_input) CloseHandle(helper_input);
        if (host_input) CloseHandle(host_input);
        if (host_output) CloseHandle(host_output);
        if (helper_output) CloseHandle(helper_output);
        if (null_error) CloseHandle(null_error);
    };
    if (!CreatePipe(&helper_input, &host_input, &security, 0) || !CreatePipe(&host_output, &helper_output, &security, 0) ||
        !SetHandleInformation(host_input, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(host_output, HANDLE_FLAG_INHERIT, 0)) {
        const auto error = windows_error("CreatePipe");
        cleanup();
        return std::unexpected(error);
    }
    null_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (null_error == INVALID_HANDLE_VALUE) {
        null_error = nullptr;
        const auto error = windows_error("opening NUL for PTY helper");
        cleanup();
        return std::unexpected(error);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = helper_input;
    startup.hStdOutput = helper_output;
    startup.hStdError = null_error;
    PROCESS_INFORMATION process{};
    auto command = L"python.exe -u \"" + helper->wstring() + L"\"";
    const auto created =
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, true, CREATE_NO_WINDOW, nullptr, directory->c_str(), &startup, &process);
    if (!created) {
        const auto error = windows_error("launching PTY helper");
        cleanup();
        return std::unexpected(error);
    }
    CloseHandle(process.hThread);
    CloseHandle(std::exchange(helper_input, nullptr));
    CloseHandle(std::exchange(helper_output, nullptr));
    CloseHandle(std::exchange(null_error, nullptr));
    state->input = std::exchange(host_input, nullptr);
    state->output_pipe = std::exchange(host_output, nullptr);
    state->helper_process = process.hProcess;
    cleanup();
    auto started = state->helper_command("START " + hex_encode(utf8(executable->wstring())) + " " + hex_encode(utf8(directory->wstring())) +
                                         " " + std::to_string(columns) + " " + std::to_string(rows));
    if (!started) {
        state->close();
        return std::unexpected(started.error());
    }
    try {
        state->process_id = std::stoull(*started);
    } catch (...) {
        state->close();
        return std::unexpected("invalid PTY helper startup response");
    }
#else
    winsize size{static_cast<unsigned short>(rows), static_cast<unsigned short>(columns), 0, 0};
    const auto child = forkpty(&state->master, nullptr, nullptr, &size);
    if (child < 0) return std::unexpected(std::string("forkpty failed: ") + std::strerror(errno));
    if (child == 0) {
        if (chdir(directory->c_str()) != 0) _exit(126);
        execl(executable->c_str(), executable->filename().c_str(), nullptr);
        _exit(127);
    }
    state->process = child;
    state->process_id = static_cast<u64>(child);
    const auto flags = fcntl(state->master, F_GETFL, 0);
    if (flags < 0 || fcntl(state->master, F_SETFL, flags | O_NONBLOCK) < 0) {
        state->close();
        return std::unexpected(std::string("configuring PTY failed: ") + std::strerror(errno));
    }
#endif

    return std::unique_ptr<LiveSession>(new LiveSession(std::move(state)));
}

std::expected<void, std::string> LiveSession::write(std::string_view bytes) {
    if (auto refreshed = impl->refresh_process(); !refreshed) return refreshed;
    if (!impl->running) return std::unexpected("Liminal process has exited");
#ifdef _WIN32
    auto response = impl->helper_command("WRITE " + hex_encode(bytes));
    if (!response) return std::unexpected(response.error());
#else
    usize offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(impl->master, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<usize>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{impl->master, POLLOUT, 0};
            if (poll(&descriptor, 1, 1000) > 0) continue;
        }
        return std::unexpected(std::string("writing PTY input failed: ") + std::strerror(errno));
    }
#endif
    return {};
}

std::expected<void, std::string> LiveSession::key(std::string_view name) {
    const auto bytes = key_bytes(name);
    if (!bytes) return std::unexpected("unsupported key: " + std::string(name));
    return write(*bytes);
}

std::expected<void, std::string> LiveSession::resize(i32 columns, i32 rows) {
#ifdef _WIN32
    auto response = impl->helper_command("RESIZE " + std::to_string(columns) + " " + std::to_string(rows));
    if (!response) return std::unexpected(response.error());
#else
    winsize size{static_cast<unsigned short>(rows), static_cast<unsigned short>(columns), 0, 0};
    if (ioctl(impl->master, TIOCSWINSZ, &size) < 0) {
        return std::unexpected(std::string("resizing PTY failed: ") + std::strerror(errno));
    }
    kill(impl->process, SIGWINCH);
#endif
    impl->columns = columns;
    impl->rows = rows;
    impl->mirror.resize(columns, rows);
    return {};
}

std::expected<void, std::string> LiveSession::wait(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (true) {
        if (auto drained = impl->drain(); !drained) return drained;
        if (auto refreshed = impl->refresh_process(); !refreshed) return refreshed;
        if (!impl->running || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return impl->drain();
}

std::expected<void, std::string> LiveSession::terminate() {
    if (auto refreshed = impl->refresh_process(); !refreshed) return refreshed;
    if (!impl->running) return {};
#ifdef _WIN32
    auto response = impl->helper_command("TERMINATE");
    if (!response) return std::unexpected(response.error());
#else
    if (kill(-impl->process, SIGTERM) < 0) {
        return std::unexpected(std::string("terminating PTY process group failed: ") + std::strerror(errno));
    }
#endif
    return wait(std::chrono::milliseconds(2000));
}

std::expected<LiveSnapshot, std::string> LiveSession::inspect() {
    if (auto drained = impl->drain(); !drained) return std::unexpected(drained.error());
    if (auto refreshed = impl->refresh_process(); !refreshed) return std::unexpected(refreshed.error());
    return LiveSnapshot{
        .columns = impl->columns,
        .rows = impl->rows,
        .running = impl->running,
        .exit_code = impl->exit_code,
        .process_id = impl->process_id,
        .output_offset = impl->output_offset,
        .output = escape_control_bytes(lighter::encoding::utf8::sanitize(impl->output)),
        .visible_text = impl->mirror.visible_text(),
        .cursor_row = impl->mirror.cursor.row,
        .cursor_column = impl->mirror.cursor.column,
        .cursor_visible = impl->mirror.cursor.visible,
    };
}

} // namespace liminal::dev_mcp
