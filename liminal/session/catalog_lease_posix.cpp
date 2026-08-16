#include "catalog_lease.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

struct CatalogLease::State {
    int file = -1;
    ~State() {
        flock(file, LOCK_UN);
        close(file);
    }
};

Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &state_root, bool exclusive) {
    std::error_code error;
    const auto directory = state_root / "locks";
    std::filesystem::create_directories(directory, error);
    if (error) return lighter::outcome_error(Error::storage("cannot create catalog lock directory: " + error.message()));
    const auto path = directory / "catalog-maintenance.lock";
    const auto file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (file < 0)
        return lighter::outcome_error(Error::storage("cannot open catalog maintenance lock: " + std::string(std::strerror(errno))));
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
    const auto path = state_root / "locks" / "catalog-initialize.lock";
    const auto file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (file < 0)
        return lighter::outcome_error(Error::storage("cannot open catalog initialization lock: " + std::string(std::strerror(errno))));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (flock(file, LOCK_EX | LOCK_NB) != 0) {
        const auto code = errno;
        if (code != EWOULDBLOCK && code != EAGAIN) {
            close(file);
            return lighter::outcome_error(
                Error::storage("cannot acquire catalog initialization lock: " + std::string(std::strerror(code))));
        }
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
