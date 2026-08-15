#include "lease.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <system_error>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session {

struct SessionLease::State {
    int file = -1;

    ~State() {
        flock(file, LOCK_UN);
        close(file);
    }
};

Result<SessionLease> acquire_session_lease(const std::filesystem::path &state_root, SessionId id) {
    const auto directory = state_root / "locks";
    std::error_code error;
    const auto created_directory = std::filesystem::create_directories(directory, error);
    if (error) return lighter::outcome_error(Error::storage("cannot create session lock directory: " + error.message()));
    if (created_directory) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
        if (error) return lighter::outcome_error(Error::storage("cannot secure session lock directory: " + error.message()));
    }
    const auto path = directory / (to_string(id) + ".lock");
    const auto file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (file < 0) return lighter::outcome_error(Error::storage("cannot open session lock file: " + std::string(std::strerror(errno))));
    if (flock(file, LOCK_EX | LOCK_NB) != 0) {
        const auto lock_error = errno;
        close(file);
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
            return lighter::outcome_error(Error::storage("session is in use by another Liminal process"));
        }
        return lighter::outcome_error(Error::storage("cannot acquire session lock: " + std::string(std::strerror(lock_error))));
    }
    auto state = std::make_shared<SessionLease::State>();
    state->file = file;
    return SessionLease(std::move(state));
}

} // namespace liminal::session

#endif
