#include "fs.h"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace lighter {

std::filesystem::path executable_path() {
#if defined(_WIN32)
    constexpr std::size_t k_max_path_length = 32768;
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= k_max_path_length) return {};
        buffer.resize(std::min(buffer.size() * 2, k_max_path_length));
    }

#elif defined(__linux__)
    constexpr std::size_t k_max_path_length = 64 * 1024;
    std::string buffer(256, '\0');
    while (true) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length <= 0) return {};
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= k_max_path_length) return {};
        buffer.resize(std::min(buffer.size() * 2, k_max_path_length));
    }
#else
#warning "executable_path() is not implemented for this platform."
    return {};
#endif
}

std::filesystem::path executable_directory() {
    const auto path = executable_path();
    if (path.empty()) return {};
    return path.parent_path();
}

} // namespace lighter
