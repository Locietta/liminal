#include "lease.h"
#include "durable_fs.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <system_error>
#include <utility>

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
    const auto file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (file < 0) return lighter::outcome_error(Error::storage("cannot open session lock file: " + std::string(std::strerror(errno))));
    struct stat info{};
    if (fstat(file, &info) != 0) {
        const auto code = errno;
        close(file);
        return lighter::outcome_error(Error::storage("cannot inspect session lock file: " + std::string(std::strerror(code))));
    }
    if (!S_ISREG(info.st_mode)) {
        close(file);
        return lighter::outcome_error(Error::storage("session lock file is not a regular file"));
    }
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
