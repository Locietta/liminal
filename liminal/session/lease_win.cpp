#include "lease.h"
#include "durable_fs.h"

#ifdef _WIN32

#include <windows.h>

#include <system_error>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session {

struct SessionLease::State {
    HANDLE file = INVALID_HANDLE_VALUE;

    ~State() {
        OVERLAPPED overlap{};
        UnlockFileEx(file, 0, 1, 0, &overlap);
        CloseHandle(file);
    }
};

Result<SessionLease> acquire_session_lease(const std::filesystem::path &state_root, SessionId id) {
    const auto directory = state_root / "locks";
    for (const auto &path : {state_root, directory}) {
        auto type = detail::inspect_path_no_follow(path);
        if (!type) return lighter::outcome_error(std::move(type).error());
        if (*type == detail::PathType::REPARSE_POINT) {
            return lighter::outcome_error(
                Error::storage("session lock path is a symlink, junction, or reparse point: " + path.generic_string()));
        }
        if (*type != detail::PathType::DIRECTORY) {
            return lighter::outcome_error(Error::storage("session lock path is not a directory: " + path.generic_string()));
        }
    }
    const auto path = directory / (to_string(id) + ".lock");
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lighter::outcome_error(
            Error::storage("cannot open session lock file: " + std::system_category().message(static_cast<int>(GetLastError()))));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        const auto code = GetLastError();
        CloseHandle(file);
        return lighter::outcome_error(
            Error::storage("cannot inspect session lock file: " + std::system_category().message(static_cast<int>(code))));
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(file);
        return lighter::outcome_error(Error::storage("session lock file is a reparse point or directory"));
    }
    OVERLAPPED overlap{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
        const auto lock_error = GetLastError();
        CloseHandle(file);
        if (lock_error == ERROR_LOCK_VIOLATION || lock_error == ERROR_IO_PENDING) {
            return lighter::outcome_error(Error::storage("session is in use by another Liminal process"));
        }
        return lighter::outcome_error(
            Error::storage("cannot acquire session lock: " + std::system_category().message(static_cast<int>(lock_error))));
    }
    auto state = std::make_shared<SessionLease::State>();
    state->file = file;
    return SessionLease(std::move(state));
}

} // namespace liminal::session

#endif
