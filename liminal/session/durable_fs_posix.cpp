#include "durable_fs.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session::detail {

namespace {

Error posix_error(std::string_view action) { return Error::storage(std::string(action) + ": " + std::strerror(errno)); }

Result<void> flush_directory(const std::filesystem::path &directory) {
    const auto descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return lighter::outcome_error(posix_error("cannot open state directory for durability"));
    const auto result = fsync(descriptor);
    const auto saved = errno;
    close(descriptor);
    errno = saved;
    if (result != 0) return lighter::outcome_error(posix_error("cannot flush state directory"));
    return {};
}

} // namespace

Result<void> durable_replace_file(const std::filesystem::path &path, std::string_view contents) {
    auto template_text = path.string() + ".tmp.XXXXXX";
    std::vector<char> template_buffer(template_text.begin(), template_text.end());
    template_buffer.push_back('\0');
    const auto descriptor = mkstemp(template_buffer.data());
    if (descriptor < 0) return lighter::outcome_error(posix_error("cannot create catalog marker temporary file"));
    const auto temporary = std::filesystem::path(template_buffer.data());
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    fchmod(descriptor, S_IRUSR | S_IWUSR);
    usize offset = 0;
    while (offset < contents.size()) {
        const auto written = write(descriptor, contents.data() + offset, contents.size() - offset);
        if (written < 0) {
            const auto saved = errno;
            close(descriptor);
            unlink(temporary.c_str());
            errno = saved;
            return lighter::outcome_error(posix_error("cannot write catalog marker"));
        }
        offset += static_cast<usize>(written);
    }
    if (fsync(descriptor) != 0) {
        const auto saved = errno;
        close(descriptor);
        unlink(temporary.c_str());
        errno = saved;
        return lighter::outcome_error(posix_error("cannot flush catalog marker"));
    }
    if (close(descriptor) != 0) {
        unlink(temporary.c_str());
        return lighter::outcome_error(posix_error("cannot close catalog marker"));
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        const auto saved = errno;
        unlink(temporary.c_str());
        errno = saved;
        return lighter::outcome_error(posix_error("cannot atomically replace catalog marker"));
    }
    return flush_directory(path.parent_path());
}

Result<void> durable_remove_file(const std::filesystem::path &path) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) return lighter::outcome_error(posix_error("cannot remove catalog marker"));
    return flush_directory(path.parent_path());
}

Result<void> publish_directory_without_replacement(const std::filesystem::path &source, const std::filesystem::path &target) {
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), 1) != 0) {
        return lighter::outcome_error(posix_error("cannot publish staged session directory"));
    }
    return flush_directory(target.parent_path());
#else
    return lighter::outcome_error(Error::storage("atomic no-replace directory publication is unsupported on this platform"));
#endif
}

Result<bool> is_reparse_point(const std::filesystem::path &path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) return lighter::outcome_error(posix_error("cannot inspect state path"));
    return S_ISLNK(info.st_mode);
}

} // namespace liminal::session::detail

#endif
