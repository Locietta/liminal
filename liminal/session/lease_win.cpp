#include "lease.h"

#ifdef _WIN32

#include <windows.h>

#include <system_error>

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

Result<SessionLease> acquire_session_lease(const std::filesystem::path &state_database, SessionId id) {
    const auto directory = state_database.parent_path() / "locks";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return lighter::outcome_error(Error::storage("cannot create session lock directory: " + error.message()));
    const auto path = directory / (to_string(id) + ".lock");
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lighter::outcome_error(
            Error::storage("cannot open session lock file: " + std::system_category().message(static_cast<int>(GetLastError()))));
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
