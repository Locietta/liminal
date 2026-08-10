#include "clipboard.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <lighter/async/io/process.h>
#include <lighter/async/io/stream.h>
#endif

#include <lighter/async/runtime/task.h>

namespace liminal::tui {

using lighter::fail;
using lighter::Task;
using namespace lighter::types;

namespace {

#ifdef _WIN32

struct ClipboardGuard {
    ~ClipboardGuard() { CloseClipboard(); }
};

Result<void> copy_native(std::string_view text) {
    if (text.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return lighter::outcome_error(Error::tool("reply is too large for the Windows clipboard"));
    }

    const auto source_size = static_cast<int>(text.size());
    const auto size = text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
    if (!text.empty() && size <= 0) return lighter::outcome_error(Error::tool("reply is not valid UTF-8"));

    std::wstring wide(static_cast<usize>(size), L'\0');
    if (!text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, wide.data(), size) != size) {
        return lighter::outcome_error(Error::tool("cannot convert the reply for the Windows clipboard"));
    }

    bool opened = false;
    for (i32 attempt = 0; attempt < 5 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(5);
    }
    if (!opened) return lighter::outcome_error(Error::tool("cannot open the Windows clipboard"));
    ClipboardGuard guard;
    if (!EmptyClipboard()) return lighter::outcome_error(Error::tool("cannot clear the Windows clipboard"));

    const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
    auto *memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return lighter::outcome_error(Error::tool("cannot allocate Windows clipboard memory"));
    auto *destination = static_cast<wchar_t *>(GlobalLock(memory));
    if (!destination) {
        GlobalFree(memory);
        return lighter::outcome_error(Error::tool("cannot lock Windows clipboard memory"));
    }
    std::ranges::copy(wide, destination);
    destination[wide.size()] = L'\0';
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        return lighter::outcome_error(Error::tool("cannot set Windows clipboard data"));
    }
    return {};
}

#else

struct ClipboardCommand {
    std::string file;
    std::vector<std::string> arguments;
};

Task<bool> try_clipboard_command(const ClipboardCommand &command, std::string_view text) {
    auto spawned = lighter::Process::spawn({
        .file = command.file,
        .args = command.arguments,
        .streams = {lighter::Process::Stdio::pipe(true, false), lighter::Process::Stdio::ignore(), lighter::Process::Stdio::ignore()},
    });
    if (!spawned) co_return false;
    auto child = *std::move(spawned);
    const auto written = co_await child.stdin_pipe.write(std::span(text.data(), text.size()));
    child.stdin_pipe = {};
    const auto status = co_await child.proc.wait();
    co_return written && status && status->status == 0 && status->term_signal == 0;
}

std::vector<ClipboardCommand> clipboard_commands() {
    std::vector<ClipboardCommand> commands;
    if (const auto *wayland = std::getenv("WAYLAND_DISPLAY"); wayland && *wayland) {
        commands.push_back({.file = "wl-copy", .arguments = {"wl-copy"}});
    }
    if (const auto *display = std::getenv("DISPLAY"); display && *display) {
        commands.push_back({.file = "xclip", .arguments = {"xclip", "-selection", "clipboard", "-in"}});
        commands.push_back({.file = "xsel", .arguments = {"xsel", "--clipboard", "--input"}});
    }
    if (const auto *wsl = std::getenv("WSL_DISTRO_NAME"); wsl && *wsl) {
        commands.push_back({.file = "clip.exe", .arguments = {"clip.exe"}});
    }
    return commands;
}

#endif

} // namespace

Task<void, Error> copy_to_clipboard(std::string text) {
#ifdef _WIN32
    auto copied = copy_native(text);
    if (!copied) co_await fail(std::move(copied).error());
#else
    for (const auto &command : clipboard_commands()) {
        if (co_await try_clipboard_command(command, text)) co_return;
    }
    co_await fail(Error::tool("no working clipboard helper found (install wl-clipboard, xclip, or xsel)"));
#endif
}

} // namespace liminal::tui
