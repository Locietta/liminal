#include "catalog_lease.h"
#include "durable_fs.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

namespace {

Result<void> validate_lock_directories(const std::filesystem::path &state_root) {
    for (const auto &directory : {state_root, state_root / "locks"}) {
        auto type = inspect_path_no_follow(directory);
        if (!type) return lighter::outcome_error(std::move(type).error());
        if (*type == PathType::REPARSE_POINT) {
            return lighter::outcome_error(
                Error::storage("catalog lock path is a symlink, junction, or reparse point: " + directory.generic_string()));
        }
        if (*type != PathType::DIRECTORY) {
            return lighter::outcome_error(Error::storage("catalog lock path is not a directory: " + directory.generic_string()));
        }
    }
    return {};
}

Result<HANDLE> open_lock_file(const std::filesystem::path &path, std::string_view description) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lighter::outcome_error(Error::storage("cannot open " + std::string(description) + ": " +
                                                     std::system_category().message(static_cast<int>(GetLastError()))));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        const auto code = GetLastError();
        CloseHandle(file);
        return lighter::outcome_error(
            Error::storage("cannot inspect catalog lock file: " + std::system_category().message(static_cast<int>(code))));
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(file);
        return lighter::outcome_error(Error::storage("catalog lock file is a reparse point or directory"));
    }
    return file;
}

} // namespace

struct CatalogLease::State {
    HANDLE file = INVALID_HANDLE_VALUE;
    ~State() {
        OVERLAPPED overlap{};
        UnlockFileEx(file, 0, 1, 0, &overlap);
        CloseHandle(file);
    }
};

Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &state_root, bool exclusive) {
    if (auto valid = validate_lock_directories(state_root); !valid) return lighter::outcome_error(std::move(valid).error());
    auto opened = open_lock_file(state_root / "locks" / "catalog-maintenance.lock", "catalog maintenance lock");
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    const auto file = *opened;
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
    if (auto valid = validate_lock_directories(state_root); !valid) return lighter::outcome_error(std::move(valid).error());
    const auto path = state_root / "locks" / "catalog-initialize.lock";
    auto opened = open_lock_file(path, "catalog initialization lock");
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    const auto file = *opened;
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
        notify_catalog_initialization_conflict();
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
