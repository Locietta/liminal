#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

struct EventLoop;

namespace fs {

using FileTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

struct FileStats {
    /// Device ID containing the file.
    u64 dev = 0;

    /// File type and mode (permissions).
    u64 mode = 0;

    /// Number of hard links.
    u64 nlink = 0;

    /// User ID of owner.
    u64 uid = 0;

    /// Group ID of owner.
    u64 gid = 0;

    /// Device ID (if special file).
    u64 rdev = 0;

    /// Inode number.
    u64 ino = 0;

    /// Total size in bytes.
    u64 size = 0;

    /// Preferred I/O block size.
    u64 blksize = 0;

    /// Number of 512-byte blocks allocated.
    u64 blocks = 0;

    /// File flags (BSD-specific).
    u64 flags = 0;

    /// File generation number (BSD-specific).
    u64 gen = 0;

    /// Last access time.
    FileTime atime;

    /// Last modification time.
    FileTime mtime;

    /// Last status change time.
    FileTime ctime;

    /// Creation (birth) time.
    FileTime birthtime;
};

struct MkstempResult {
    i32 fd = -1;
    std::string path;
};

struct Dirent {
    enum class Type {
        UNKNOWN,     // type not known
        FILE,        // regular file
        DIR,         // directory
        LINK,        // symlink
        FIFO,        // FIFO/Pipe
        SOCKET,      // socket
        CHAR_DEVICE, // character device
        BLOCK_DEVICE // block device
    };
    std::string name;
    Type kind = Type::UNKNOWN;
};

struct CopyfileOptions {
    /// Fail if destination exists.
    bool excl = false;

    /// Try to clone via copy-on-write if supported.
    bool clone = false;

    /// Force clone (may fall back to copy on failure).
    bool clone_force = false;
};

struct DirHandle {
    DirHandle() = default;
    DirHandle(DirHandle &&other) noexcept;
    DirHandle &operator=(DirHandle &&other) noexcept;

    DirHandle(const DirHandle &) = delete;
    DirHandle &operator=(const DirHandle &) = delete;

    bool valid() const noexcept;
    void *native_handle() const noexcept;
    void reset() noexcept;

    static DirHandle from_native(void *ptr);

private:
    explicit DirHandle(void *ptr);

    void *dir = nullptr;
};

/// Remove a file.
Task<void, Error> unlink(std::string_view path, EventLoop &loop = EventLoop::current());

/// Create a directory with the given permissions.
Task<void, Error> mkdir(std::string_view path, i32 mode, EventLoop &loop = EventLoop::current());

/// Get file status (metadata) by path.
Task<FileStats, Error> stat(std::string_view path, EventLoop &loop = EventLoop::current());

/// Copy a file from path to new_path.
Task<void, Error> copyfile(std::string_view path, std::string_view new_path, CopyfileOptions options = CopyfileOptions{},
                           EventLoop &loop = EventLoop::current());

/// Create a unique temporary directory from a template (must end with "XXXXXX").
Task<std::string, Error> mkdtemp(std::string_view tpl, EventLoop &loop = EventLoop::current());

/// Create a unique temporary file from a template (must end with "XXXXXX").
Task<MkstempResult, Error> mkstemp(std::string_view tpl, EventLoop &loop = EventLoop::current());

/// Remove an empty directory.
Task<void, Error> rmdir(std::string_view path, EventLoop &loop = EventLoop::current());

/// Scan a directory, returning all entries at once.
Task<std::vector<Dirent>, Error> scandir(std::string_view path, EventLoop &loop = EventLoop::current());

/// Open a directory for iterative reading.
Task<DirHandle, Error> opendir(std::string_view path, EventLoop &loop = EventLoop::current());

/// Read a batch of entries from an opened directory.
Task<std::vector<Dirent>, Error> readdir(DirHandle &dir, EventLoop &loop = EventLoop::current());

/// Close an opened directory handle.
Task<void, Error> closedir(DirHandle &dir, EventLoop &loop = EventLoop::current());

/// Get file status by file descriptor.
Task<FileStats, Error> fstat(i32 fd, EventLoop &loop = EventLoop::current());

/// Get file status by path, without following symlinks.
Task<FileStats, Error> lstat(std::string_view path, EventLoop &loop = EventLoop::current());

/// Rename (move) a file or directory.
Task<void, Error> rename(std::string_view path, std::string_view new_path, EventLoop &loop = EventLoop::current());

/// Flush file data and metadata to disk.
Task<void, Error> fsync(i32 fd, EventLoop &loop = EventLoop::current());

/// Flush file data to disk (metadata may not be flushed).
Task<void, Error> fdatasync(i32 fd, EventLoop &loop = EventLoop::current());

/// Truncate a file to the specified length.
Task<void, Error> ftruncate(i32 fd, i64 offset, EventLoop &loop = EventLoop::current());

/// Zero-copy transfer data between file descriptors.
Task<i64, Error> sendfile(i32 out_fd, i32 in_fd, i64 in_offset, usize length, EventLoop &loop = EventLoop::current());

/// Check user permissions for a file (mode: F_OK, R_OK, W_OK, X_OK).
Task<void, Error> access(std::string_view path, i32 mode, EventLoop &loop = EventLoop::current());

/// Change file permissions by path.
Task<void, Error> chmod(std::string_view path, i32 mode, EventLoop &loop = EventLoop::current());

/// Change file access and modification times by path.
Task<void, Error> utime(std::string_view path, f64 atime, f64 mtime, EventLoop &loop = EventLoop::current());

/// Change file access and modification times by file descriptor.
Task<void, Error> futime(i32 fd, f64 atime, f64 mtime, EventLoop &loop = EventLoop::current());

/// Change file access and modification times by path, without following symlinks.
Task<void, Error> lutime(std::string_view path, f64 atime, f64 mtime, EventLoop &loop = EventLoop::current());

/// Create a hard link.
Task<void, Error> link(std::string_view path, std::string_view new_path, EventLoop &loop = EventLoop::current());

/// Create a symbolic link.
Task<void, Error> symlink(std::string_view path, std::string_view new_path, i32 flags = 0, EventLoop &loop = EventLoop::current());

/// Read the target of a symbolic link.
Task<std::string, Error> readlink(std::string_view path, EventLoop &loop = EventLoop::current());

/// Resolve a path to its canonical absolute pathname.
Task<std::string, Error> realpath(std::string_view path, EventLoop &loop = EventLoop::current());

/// Change file permissions by file descriptor.
Task<void, Error> fchmod(i32 fd, i32 mode, EventLoop &loop = EventLoop::current());

/// Change file owner and group by path.
Task<void, Error> chown(std::string_view path, u32 uid, u32 gid, EventLoop &loop = EventLoop::current());

/// Change file owner and group by file descriptor.
Task<void, Error> fchown(i32 fd, u32 uid, u32 gid, EventLoop &loop = EventLoop::current());

/// Change file owner and group by path, without following symlinks.
Task<void, Error> lchown(std::string_view path, u32 uid, u32 gid, EventLoop &loop = EventLoop::current());

struct FsStats {
    /// Filesystem type identifier.
    u64 type = 0;

    /// Fundamental block size in bytes.
    u64 bsize = 0;

    /// Total number of blocks on the filesystem.
    u64 blocks = 0;

    /// Number of free blocks.
    u64 bfree = 0;

    /// Number of free blocks available to unprivileged users.
    u64 bavail = 0;

    /// Total number of file nodes (inodes).
    u64 files = 0;

    /// Number of free file nodes.
    u64 ffree = 0;

    /// Fragment size in bytes.
    u64 frsize = 0;
};

/// Get filesystem statistics (total/free space, inode counts, etc.).
Task<FsStats, Error> statfs(std::string_view path, EventLoop &loop = EventLoop::current());

/// Open a file asynchronously. Returns the file descriptor on success.
Task<i32, Error> open(std::string_view path, i32 flags, i32 mode = 0, EventLoop &loop = EventLoop::current());

/// Read from a file descriptor into a buffer. offset = -1 uses current position.
Task<usize, Error> read(i32 fd, std::span<char> buf, i64 offset = -1, EventLoop &loop = EventLoop::current());

/// Write a buffer to a file descriptor. offset = -1 uses current position.
Task<usize, Error> write(i32 fd, std::span<const char> buf, i64 offset = -1, EventLoop &loop = EventLoop::current());

/// Close a file descriptor asynchronously.
Task<void, Error> close(i32 fd, EventLoop &loop = EventLoop::current());

namespace sync {

/// Open a file. Returns the fd on success.
/// flags: UV_FS_O_RDONLY, UV_FS_O_WRONLY, UV_FS_O_RDWR, etc.
Result<i32> open(std::string_view path, i32 flags, i32 mode = 0);

/// Read up to buf.size() bytes from fd at offset (-1 = current position).
Result<usize> read(i32 fd, std::span<char> buf, i64 offset = -1);

/// Write buf to fd at offset (-1 = current position).
Result<usize> write(i32 fd, std::span<const char> buf, i64 offset = -1);

/// Close a file descriptor.
Error close(i32 fd);

/// Convenience: read entire file into a string.
Result<std::string> read_to_string(std::string_view path);

} // namespace sync

} // namespace fs

} // namespace lighter
