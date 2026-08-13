#include "paths.h"

#ifdef _WIN32

#include <limits>
#include <system_error>

#include <windows.h>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

namespace {

Error windows_error(std::string_view action) {
    return Error::storage(std::string(action) + ": " + std::system_category().message(static_cast<int>(GetLastError())));
}

} // namespace

Result<std::string> windows_workspace_key(const std::filesystem::path &path) {
    auto native = path.native();
    for (auto &character : native) {
        if (character == L'\\') character = L'/';
    }
    if (native.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return lighter::outcome_error(Error::storage("workspace path is too long to normalize"));
    }

    const auto source_size = static_cast<int>(native.size());
    const auto mapped_size =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), source_size, nullptr, 0, nullptr, nullptr, 0);
    if (mapped_size == 0) return lighter::outcome_error(windows_error("cannot case-normalize workspace path"));
    std::wstring mapped(static_cast<usize>(mapped_size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), source_size, mapped.data(), mapped_size, nullptr, nullptr,
                      0) == 0) {
        return lighter::outcome_error(windows_error("cannot case-normalize workspace path"));
    }

    const auto encoded_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, mapped.data(), mapped_size, nullptr, 0, nullptr, nullptr);
    if (encoded_size == 0) return lighter::outcome_error(windows_error("cannot encode normalized workspace path"));
    std::string encoded(static_cast<usize>(encoded_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, mapped.data(), mapped_size, encoded.data(), encoded_size, nullptr, nullptr) ==
        0) {
        return lighter::outcome_error(windows_error("cannot encode normalized workspace path"));
    }
    return encoded;
}

} // namespace liminal::session::detail

#endif
