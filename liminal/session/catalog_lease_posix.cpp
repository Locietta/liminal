#include "catalog_lease.h"
#include "durable_fs.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
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

Result<int> open_lock_file(const std::filesystem::path &path, std::string_view description) {
    const auto file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (file < 0) {
        return lighter::outcome_error(Error::storage("cannot open " + std::string(description) + ": " + std::strerror(errno)));
    }
    struct stat info{};
    if (fstat(file, &info) != 0) {
        const auto code = errno;
        close(file);
        return lighter::outcome_error(Error::storage("cannot inspect catalog lock file: " + std::string(std::strerror(code))));
    }
    if (!S_ISREG(info.st_mode)) {
        close(file);
        return lighter::outcome_error(Error::storage("catalog lock file is not a regular file"));
    }
    return file;
}

} // namespace

struct CatalogLease::State {
    int file = -1;
    ~State() {
        flock(file, LOCK_UN);
        close(file);
    }
};

Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &state_root, bool exclusive) {
    if (auto valid = validate_lock_directories(state_root); !valid) return lighter::outcome_error(std::move(valid).error());
    auto opened = open_lock_file(state_root / "locks" / "catalog-maintenance.lock", "catalog maintenance lock");
    if (!opened) return lighter::outcome_error(std::move(opened).error());
    const auto file = *opened;
    if (flock(file, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        const auto code = errno;
        close(file);
        if (code == EWOULDBLOCK || code == EAGAIN) {
            return lighter::outcome_error(Error::storage(exclusive ? "catalog repair requires every Liminal process to close the catalog" :
                                                                     "session catalog is under exclusive maintenance"));
        }
        return lighter::outcome_error(Error::storage("cannot acquire catalog maintenance lock: " + std::string(std::strerror(code))));
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
    while (flock(file, LOCK_EX | LOCK_NB) != 0) {
        const auto code = errno;
        if (code != EWOULDBLOCK && code != EAGAIN) {
            close(file);
            return lighter::outcome_error(
                Error::storage("cannot acquire catalog initialization lock: " + std::string(std::strerror(code))));
        }
        notify_catalog_initialization_conflict();
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            close(file);
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
