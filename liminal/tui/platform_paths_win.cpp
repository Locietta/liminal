#include "platform_paths.h"

#ifdef _WIN32

#include <limits>
#include <string_view>
#include <system_error>

#include <windows.h>

#include <lighter/async/vocab/outcome.h>
#include <lighter/types.hpp>

namespace liminal::tui {

using namespace lighter::types;

namespace {

Error windows_error(std::string_view action) {
    return Error::config(std::string(action) + ": " + std::system_category().message(static_cast<int>(GetLastError())));
}

Result<std::string> utf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return lighter::outcome_error(Error::config("native path is too long to encode as UTF-8"));
    }
    const auto size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, nullptr, 0, nullptr, nullptr);
    if (required == 0) return lighter::outcome_error(windows_error("cannot measure native UTF-8 text"));
    std::string result(static_cast<usize>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, result.data(), required, nullptr, nullptr) == 0) {
        return lighter::outcome_error(windows_error("cannot encode native UTF-8 text"));
    }
    return result;
}

Result<std::optional<std::wstring>> environment(const wchar_t *name) {
    SetLastError(ERROR_SUCCESS);
    auto required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        const auto error = GetLastError();
        if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return lighter::outcome_error(windows_error("cannot read user environment"));
    }

    std::wstring value(static_cast<usize>(required), L'\0');
    const auto written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0) return lighter::outcome_error(windows_error("cannot read user environment"));
    if (written >= required) return lighter::outcome_error(Error::config("user environment changed while it was being read"));
    value.resize(static_cast<usize>(written));
    return std::optional(std::move(value));
}

} // namespace

Result<std::string> native_path_utf8(const std::filesystem::path &path) { return utf8(path.native()); }

Result<std::optional<std::string>> user_home_directory_utf8() {
    auto profile = environment(L"USERPROFILE");
    if (!profile) return lighter::outcome_error(std::move(profile).error());
    if (*profile) {
        auto encoded = utf8(**profile);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        return std::optional(*std::move(encoded));
    }

    auto drive = environment(L"HOMEDRIVE");
    if (!drive) return lighter::outcome_error(std::move(drive).error());
    auto path = environment(L"HOMEPATH");
    if (!path) return lighter::outcome_error(std::move(path).error());
    if (*drive && *path) {
        auto encoded = utf8(**drive + **path);
        if (!encoded) return lighter::outcome_error(std::move(encoded).error());
        return std::optional(*std::move(encoded));
    }

    auto home = environment(L"HOME");
    if (!home) return lighter::outcome_error(std::move(home).error());
    if (!*home) return std::nullopt;
    auto encoded = utf8(**home);
    if (!encoded) return lighter::outcome_error(std::move(encoded).error());
    return std::optional(*std::move(encoded));
}

} // namespace liminal::tui

#endif
