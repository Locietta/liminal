#include "durable_fs.h"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <system_error>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

namespace {

Error windows_error(std::string_view action) {
    return Error::storage(std::string(action) + ": " + std::system_category().message(static_cast<int>(GetLastError())));
}

std::filesystem::path temporary_path(const std::filesystem::path &target) {
    std::array<wchar_t, 32> suffix{};
    _snwprintf_s(suffix.data(), suffix.size(), _TRUNCATE, L".tmp.%lu.%llu", GetCurrentProcessId(),
                 static_cast<unsigned long long>(GetTickCount64()));
    return target.native() + suffix.data();
}

} // namespace

Result<void> durable_replace_file(const std::filesystem::path &path, std::string_view contents) {
    const auto temporary = temporary_path(path);
    const auto file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return lighter::outcome_error(windows_error("cannot create catalog marker temporary file"));
    DWORD written = 0;
    const auto size = static_cast<DWORD>(contents.size());
    const bool wrote = WriteFile(file, contents.data(), size, &written, nullptr) != 0 && written == size;
    const bool flushed = wrote && FlushFileBuffers(file) != 0;
    const auto close_ok = CloseHandle(file) != 0;
    if (!flushed || !close_ok) {
        const auto error = windows_error("cannot durably write catalog marker");
        DeleteFileW(temporary.c_str());
        return lighter::outcome_error(error);
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = windows_error("cannot atomically replace catalog marker");
        DeleteFileW(temporary.c_str());
        return lighter::outcome_error(error);
    }
    return {};
}

Result<void> durable_remove_file(const std::filesystem::path &path) {
    if (DeleteFileW(path.c_str())) return {};
    if (GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    return lighter::outcome_error(windows_error("cannot remove catalog marker"));
}

Result<void> rename_directory_without_replacement(const std::filesystem::path &source, const std::filesystem::path &target) {
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return lighter::outcome_error(windows_error("cannot publish staged session directory"));
    }
    return {};
}

Result<void> flush_published_directory(const std::filesystem::path &) { return {}; }

Result<bool> is_reparse_point(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return lighter::outcome_error(windows_error("cannot inspect state path"));
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

} // namespace liminal::session::detail

#endif
