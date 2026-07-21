#include "fs.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <linux/limits.h>
#endif

namespace lighter {

std::filesystem::path executable_path() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const auto length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length <= 0) return {};
    buffer[length] = L'\0';
    return std::filesystem::path(buffer);

#elif defined(__linux__)
    char buffer[PATH_MAX];
    const auto length = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (length <= 0) return {};
    buffer[length] = '\0';
    return std::filesystem::path(buffer);
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