#include "catalog_lease.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

struct CatalogLease::State {
    HANDLE file = INVALID_HANDLE_VALUE;
    ~State() {
        OVERLAPPED overlap{};
        UnlockFileEx(file, 0, 1, 0, &overlap);
        CloseHandle(file);
    }
};

Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &state_root, bool exclusive) {
    std::error_code error;
    const auto directory = state_root / "locks";
    std::filesystem::create_directories(directory, error);
    if (error) return lighter::outcome_error(Error::storage("cannot create catalog lock directory: " + error.message()));
    const auto path = directory / "catalog-maintenance.lock";
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lighter::outcome_error(
            Error::storage("cannot open catalog maintenance lock: " + std::system_category().message(static_cast<int>(GetLastError()))));
    }
    OVERLAPPED overlap{};
    const auto flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(file, flags, 0, 1, 0, &overlap)) {
        const auto code = GetLastError();
        CloseHandle(file);
        if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
            return lighter::outcome_error(Error::storage(exclusive ? "catalog repair requires every Liminal process to close the catalog" :
                                                                     "session catalog is under exclusive maintenance"));
        }
        return lighter::outcome_error(
            Error::storage("cannot acquire catalog maintenance lock: " + std::system_category().message(static_cast<int>(code))));
    }
    auto state = std::make_shared<CatalogLease::State>();
    state->file = file;
    return CatalogLease(std::move(state));
}

Result<CatalogLease> acquire_catalog_initialization_lease(const std::filesystem::path &state_root, std::chrono::milliseconds timeout) {
    using namespace std::chrono_literals;
    const auto path = state_root / "locks" / "catalog-initialize.lock";
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lighter::outcome_error(
            Error::storage("cannot open catalog initialization lock: " + std::system_category().message(static_cast<int>(GetLastError()))));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        OVERLAPPED overlap{};
        if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) break;
        const auto code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING) {
            CloseHandle(file);
            return lighter::outcome_error(
                Error::storage("cannot acquire catalog initialization lock: " + std::system_category().message(static_cast<int>(code))));
        }
        if (code == ERROR_IO_PENDING) CancelIoEx(file, &overlap);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            CloseHandle(file);
            return lighter::outcome_error(Error::storage("session catalog initialization is already in progress"));
        }
        std::this_thread::sleep_for(std::min(10ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
    auto state = std::make_shared<CatalogLease::State>();
    state->file = file;
    return CatalogLease(std::move(state));
}

} // namespace liminal::session::detail

#endif
