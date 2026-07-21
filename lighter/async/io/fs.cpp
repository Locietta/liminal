#include "fs.h"

#include <cassert>
#include <concepts>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

// ============================================================================
// Filesystem operations
// ============================================================================

namespace {

template <typename Populate, typename FsResult>
concept fs_result_populator = std::move_constructible<Populate> && requires(Populate &populate, uv_fs_t &req) {
    { populate(req) } -> std::convertible_to<Result<FsResult>>;
};

template <typename FsResult, fs_result_populator<FsResult> Populate> struct FsOp : uv::AwaitOp<FsOp<FsResult, Populate>> {
    using promise_t = Task<FsResult, Error>::promise_type;

    uv_fs_t req = {};
    Populate populate;
    Result<FsResult> out = outcome_error(Error());

    explicit FsOp(Populate &&populate) : populate(std::move(populate)) {}

    static void on_cancel(IoOp *op) {
        auto *self = static_cast<FsOp *>(op);
        uv::cancel(self->req);
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        return this->attach(waiting.promise(), loc);
    }

    Result<FsResult> await_resume() noexcept { return std::move(out); }
};

static fs::Dirent::Type map_dirent(uv_dirent_type_t t) {
    switch (t) {
        case UV_DIRENT_FILE: return fs::Dirent::Type::FILE;
        case UV_DIRENT_DIR: return fs::Dirent::Type::DIR;
        case UV_DIRENT_LINK: return fs::Dirent::Type::LINK;
        case UV_DIRENT_FIFO: return fs::Dirent::Type::FIFO;
        case UV_DIRENT_SOCKET: return fs::Dirent::Type::SOCKET;
        case UV_DIRENT_CHAR: return fs::Dirent::Type::CHAR_DEVICE;
        case UV_DIRENT_BLOCK: return fs::Dirent::Type::BLOCK_DEVICE;
        default: return fs::Dirent::Type::UNKNOWN;
    }
}

static Result<i32> to_uv_copyfile_flags(const fs::CopyfileOptions &options) {
    u32 out = 0;
#ifdef UV_FS_COPYFILE_EXCL
    if (options.excl) {
        out |= UV_FS_COPYFILE_EXCL;
    }
#else
    if (options.excl) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_FS_COPYFILE_FICLONE
    if (options.clone) {
        out |= UV_FS_COPYFILE_FICLONE;
    }
#else
    if (options.clone) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
#ifdef UV_FS_COPYFILE_FICLONE_FORCE
    if (options.clone_force) {
        out |= UV_FS_COPYFILE_FICLONE_FORCE;
    }
#else
    if (options.clone_force) {
        return outcome_error(Error::k_function_not_implemented);
    }
#endif
    return static_cast<i32>(out);
}

template <typename Result, typename Submit, fs_result_populator<Result> Populate>
static Task<Result, Error> run_fs(Submit submit, Populate populate, [[maybe_unused]] EventLoop &loop = EventLoop::current()) {
    using Op = FsOp<Result, Populate>;
    Op op{std::move(populate)};

    auto after_cb = [](uv_fs_t *req) {
        auto *h = static_cast<Op *>(req->data);
        assert(h != nullptr && "fs after_cb requires operation in req->data");

        h->mark_cancelled_if(req->result);

        if (req->result < 0) {
            h->out = outcome_error(uv::status_to_error(req->result));
        } else {
            h->out = h->populate(*req);
        }

        uv::fs_req_cleanup(*req);

        h->complete();
    };

    op.req.data = &op;

    if (auto err = submit(op.req, after_cb)) {
        // Callback won't fire on submit failure; clean up manually.
        uv::fs_req_cleanup(op.req);
        co_await fail(err);
    }

    co_return co_await op;
}

template <typename Submit> static Task<void, Error> run_void_fs(Submit submit, [[maybe_unused]] EventLoop &loop) {
    if (auto res = co_await run_fs<i32>(std::move(submit), [](uv_fs_t &) { return 0; }, loop); !res) {
        co_await fail(res.error());
    }
}

static fs::FileTime to_file_time(const uv_timespec_t &ts) {
    return fs::FileTime{std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}};
}

static fs::FileStats to_file_stats(const uv_stat_t &s) {
    return {
        .dev = s.st_dev,
        .mode = s.st_mode,
        .nlink = s.st_nlink,
        .uid = s.st_uid,
        .gid = s.st_gid,
        .rdev = s.st_rdev,
        .ino = s.st_ino,
        .size = s.st_size,
        .blksize = s.st_blksize,
        .blocks = s.st_blocks,
        .flags = s.st_flags,
        .gen = s.st_gen,
        .atime = to_file_time(s.st_atim),
        .mtime = to_file_time(s.st_mtim),
        .ctime = to_file_time(s.st_ctim),
        .birthtime = to_file_time(s.st_birthtim),
    };
}

} // namespace

// ============================================================================
// DirHandle
// ============================================================================

fs::DirHandle::DirHandle(DirHandle &&other) noexcept : dir(other.dir) { other.dir = nullptr; }

fs::DirHandle &fs::DirHandle::operator=(DirHandle &&other) noexcept {
    // FIXME: Should we close the existing dir handle if valid?
    if (this != &other) {
        dir = other.dir;
        other.dir = nullptr;
    }
    return *this;
}

fs::DirHandle::DirHandle(void *ptr) : dir(ptr) {}

bool fs::DirHandle::valid() const noexcept { return dir != nullptr; }

void *fs::DirHandle::native_handle() const noexcept { return dir; }

void fs::DirHandle::reset() noexcept { dir = nullptr; }

fs::DirHandle fs::DirHandle::from_native(void *ptr) { return DirHandle(ptr); }

// ============================================================================
// Success/failure operations
// ============================================================================

Task<void, Error> fs::unlink(std::string_view path, EventLoop &loop) {
    return run_void_fs([p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_unlink(loop, req, p.c_str(), cb); }, loop);
}

Task<void, Error> fs::mkdir(std::string_view path, i32 mode, EventLoop &loop) {
    return run_void_fs(
        [p = std::string(path), mode, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_mkdir(loop, req, p.c_str(), mode, cb); }, loop);
}

Task<void, Error> fs::rmdir(std::string_view path, EventLoop &loop) {
    return run_void_fs([p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_rmdir(loop, req, p.c_str(), cb); }, loop);
}

Task<void, Error> fs::fsync(i32 fd, EventLoop &loop) {
    return run_void_fs([fd, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_fsync(loop, req, fd, cb); }, loop);
}

Task<void, Error> fs::fdatasync(i32 fd, EventLoop &loop) {
    return run_void_fs([fd, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_fdatasync(loop, req, fd, cb); }, loop);
}

Task<void, Error> fs::ftruncate(i32 fd, i64 offset, EventLoop &loop) {
    return run_void_fs([fd, offset, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_ftruncate(loop, req, fd, offset, cb); }, loop);
}

Task<void, Error> fs::access(std::string_view path, i32 mode, EventLoop &loop) {
    return run_void_fs(
        [p = std::string(path), mode, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_access(loop, req, p.c_str(), mode, cb); }, loop);
}

Task<void, Error> fs::chmod(std::string_view path, i32 mode, EventLoop &loop) {
    return run_void_fs(
        [p = std::string(path), mode, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_chmod(loop, req, p.c_str(), mode, cb); }, loop);
}

Task<void, Error> fs::utime(std::string_view path, f64 atime, f64 mtime, EventLoop &loop) {
    return run_void_fs([p = std::string(path), atime, mtime,
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_utime(loop, req, p.c_str(), atime, mtime, cb); },
                       loop);
}

Task<void, Error> fs::futime(i32 fd, f64 atime, f64 mtime, EventLoop &loop) {
    return run_void_fs([fd, atime, mtime, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_futime(loop, req, fd, atime, mtime, cb); },
                       loop);
}

Task<void, Error> fs::lutime(std::string_view path, f64 atime, f64 mtime, EventLoop &loop) {
    return run_void_fs([p = std::string(path), atime, mtime,
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_lutime(loop, req, p.c_str(), atime, mtime, cb); },
                       loop);
}

Task<void, Error> fs::copyfile(std::string_view path, std::string_view new_path, fs::CopyfileOptions options, EventLoop &loop) {
    auto uv_flags = co_await or_fail(to_uv_copyfile_flags(options));

    co_await run_void_fs([p = std::string(path), np = std::string(new_path), uv_flags,
                          &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_copyfile(loop, req, p.c_str(), np.c_str(), uv_flags, cb); },
                         loop)
        .or_fail();
}

Task<void, Error> fs::rename(std::string_view path, std::string_view new_path, EventLoop &loop) {
    return run_void_fs([p = std::string(path), np = std::string(new_path),
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_rename(loop, req, p.c_str(), np.c_str(), cb); },
                       loop);
}

Task<void, Error> fs::link(std::string_view path, std::string_view new_path, EventLoop &loop) {
    return run_void_fs([p = std::string(path), np = std::string(new_path),
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_link(loop, req, p.c_str(), np.c_str(), cb); },
                       loop);
}

Task<void, Error> fs::symlink(std::string_view path, std::string_view new_path, i32 flags, EventLoop &loop) {
    return run_void_fs([p = std::string(path), np = std::string(new_path), flags,
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_symlink(loop, req, p.c_str(), np.c_str(), flags, cb); },
                       loop);
}

Task<void, Error> fs::fchmod(i32 fd, i32 mode, EventLoop &loop) {
    return run_void_fs([fd, mode, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_fchmod(loop, req, fd, mode, cb); }, loop);
}

Task<void, Error> fs::chown(std::string_view path, u32 uid, u32 gid, EventLoop &loop) {
    return run_void_fs(
        [p = std::string(path), uid, gid, &loop](uv_fs_t &req, uv_fs_cb cb) {
            return uv::fs_chown(loop, req, p.c_str(), static_cast<uv_uid_t>(uid), static_cast<uv_gid_t>(gid), cb);
        },
        loop);
}

Task<void, Error> fs::fchown(i32 fd, u32 uid, u32 gid, EventLoop &loop) {
    return run_void_fs(
        [fd, uid, gid, &loop](uv_fs_t &req, uv_fs_cb cb) {
            return uv::fs_fchown(loop, req, fd, static_cast<uv_uid_t>(uid), static_cast<uv_gid_t>(gid), cb);
        },
        loop);
}

Task<void, Error> fs::lchown(std::string_view path, u32 uid, u32 gid, EventLoop &loop) {
    return run_void_fs(
        [p = std::string(path), uid, gid, &loop](uv_fs_t &req, uv_fs_cb cb) {
            return uv::fs_lchown(loop, req, p.c_str(), static_cast<uv_uid_t>(uid), static_cast<uv_gid_t>(gid), cb);
        },
        loop);
}

Task<void, Error> fs::close(i32 fd, EventLoop &loop) {
    return run_void_fs([fd, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_close(loop, req, fd, cb); }, loop);
}

Task<void, Error> fs::closedir(fs::DirHandle &dir, EventLoop &loop) {
    if (!dir.valid()) {
        co_await fail(Error::k_invalid_argument);
    }

    co_await run_void_fs(
        [&](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_closedir(loop, req, *static_cast<uv_dir_t *>(dir.native_handle()), cb); }, loop)
        .or_fail();
    dir.reset();
}

// ============================================================================
// Stat operations
// ============================================================================

Task<fs::FileStats, Error> fs::stat(std::string_view path, EventLoop &loop) {
    return run_fs<fs::FileStats>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_stat(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) { return to_file_stats(req.statbuf); }, loop);
}

Task<fs::FileStats, Error> fs::fstat(i32 fd, EventLoop &loop) {
    return run_fs<fs::FileStats>([fd, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_fstat(loop, req, fd, cb); },
                                 [](uv_fs_t &req) { return to_file_stats(req.statbuf); }, loop);
}

Task<fs::FileStats, Error> fs::lstat(std::string_view path, EventLoop &loop) {
    return run_fs<fs::FileStats>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_lstat(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) { return to_file_stats(req.statbuf); }, loop);
}

// ============================================================================
// Operations with typed results
// ============================================================================

Task<std::string, Error> fs::mkdtemp(std::string_view tpl, EventLoop &loop) {
    return run_fs<std::string>(
        [t = std::string(tpl), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_mkdtemp(loop, req, t.c_str(), cb); },
        [](uv_fs_t &req) -> std::string { return req.path ? req.path : ""; }, loop);
}

Task<fs::MkstempResult, Error> fs::mkstemp(std::string_view tpl, EventLoop &loop) {
    return run_fs<fs::MkstempResult>(
        [t = std::string(tpl), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_mkstemp(loop, req, t.c_str(), cb); },
        [](uv_fs_t &req) -> fs::MkstempResult { return {static_cast<i32>(req.result), req.path ? req.path : ""}; }, loop);
}

Task<i64, Error> fs::sendfile(i32 out_fd, i32 in_fd, i64 in_offset, usize length, EventLoop &loop) {
    return run_fs<i64>([out_fd, in_fd, in_offset, length,
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_sendfile(loop, req, out_fd, in_fd, in_offset, length, cb); },
                       [](uv_fs_t &req) -> i64 { return req.result; }, loop);
}

Task<std::string, Error> fs::readlink(std::string_view path, EventLoop &loop) {
    return run_fs<std::string>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_readlink(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) -> Result<std::string> {
            if (!req.ptr) {
                return outcome_error(Error::k_io_error);
            }
            return std::string(static_cast<const char *>(req.ptr));
        },
        loop);
}

Task<std::string, Error> fs::realpath(std::string_view path, EventLoop &loop) {
    return run_fs<std::string>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_realpath(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) -> Result<std::string> {
            if (!req.ptr) {
                return outcome_error(Error::k_io_error);
            }
            return std::string(static_cast<const char *>(req.ptr));
        },
        loop);
}

Task<fs::FsStats, Error> fs::statfs(std::string_view path, EventLoop &loop) {
    return run_fs<fs::FsStats>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_statfs(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) -> Result<fs::FsStats> {
            auto *s = static_cast<uv_statfs_t *>(req.ptr);
            if (!s) {
                return outcome_error(Error::k_io_error);
            }
            return fs::FsStats{
                .type = s->f_type,
                .bsize = s->f_bsize,
                .blocks = s->f_blocks,
                .bfree = s->f_bfree,
                .bavail = s->f_bavail,
                .files = s->f_files,
                .ffree = s->f_ffree,
        // f_frsize was added in libuv 1.52. Fall back to f_bsize on older
        // versions (conda-forge's macOS toolchain still ships 1.51).
#if UV_VERSION_HEX >= ((1 << 16) | (52 << 8))
                .frsize = s->f_frsize,
#else
                .frsize = s->f_bsize,
#endif
            };
        },
        loop);
}

Task<i32, Error> fs::open(std::string_view path, i32 flags, i32 mode, EventLoop &loop) {
    return run_fs<i32>([p = std::string(path), flags, mode,
                        &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_open(loop, req, p.c_str(), flags, mode, cb); },
                       [](uv_fs_t &req) -> i32 { return static_cast<i32>(req.result); }, loop);
}

Task<usize, Error> fs::read(i32 fd, std::span<char> buf, i64 offset, EventLoop &loop) {
    auto storage = std::make_shared<uv_buf_t>(uv_buf_init(buf.data(), static_cast<u32>(buf.size())));

    return run_fs<usize>(
        [fd, storage, offset, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_read(loop, req, fd, storage.get(), 1, offset, cb); },
        [](uv_fs_t &req) -> usize { return static_cast<usize>(req.result); }, loop);
}

Task<usize, Error> fs::write(i32 fd, std::span<const char> buf, i64 offset, EventLoop &loop) {
    auto storage = std::make_shared<uv_buf_t>(uv_buf_init(const_cast<char *>(buf.data()), static_cast<u32>(buf.size())));

    return run_fs<usize>(
        [fd, storage, offset, &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_write(loop, req, fd, storage.get(), 1, offset, cb); },
        [](uv_fs_t &req) -> usize { return static_cast<usize>(req.result); }, loop);
}

// ============================================================================
// Directory enumeration
// ============================================================================

Task<std::vector<fs::Dirent>, Error> fs::scandir(std::string_view path, EventLoop &loop) {
    return run_fs<std::vector<fs::Dirent>>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_scandir(loop, req, p.c_str(), 0, cb); },
        [](uv_fs_t &req) -> Result<std::vector<fs::Dirent>> {
            std::vector<fs::Dirent> out;
            uv_dirent_t ent;
            while (true) {
                auto err = uv::fs_scandir_next(req, ent);
                if (err == Error::k_end_of_file) {
                    break;
                }
                if (err) {
                    return Result<std::vector<fs::Dirent>>(outcome_error(err));
                }

                fs::Dirent d;
                if (ent.name) {
                    d.name = ent.name;
                }
                d.kind = map_dirent(ent.type);
                out.push_back(std::move(d));
            }
            return out;
        },
        loop);
}

Task<fs::DirHandle, Error> fs::opendir(std::string_view path, EventLoop &loop) {
    return run_fs<fs::DirHandle>(
        [p = std::string(path), &loop](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_opendir(loop, req, p.c_str(), cb); },
        [](uv_fs_t &req) { return fs::DirHandle::from_native(req.ptr); }, loop);
}

Task<std::vector<fs::Dirent>, Error> fs::readdir(fs::DirHandle &dir, EventLoop &loop) {
    if (!dir.valid()) {
        co_await fail(Error::k_invalid_argument);
    }

    auto dir_ptr = static_cast<uv_dir_t *>(dir.native_handle());
    if (dir_ptr == nullptr) {
        co_await fail(Error::k_invalid_argument);
    }

    constexpr usize entry_count = 64;
    auto entries_storage = std::make_shared<std::vector<uv_dirent_t>>(entry_count);
    dir_ptr->dirents = entries_storage->data();
    dir_ptr->nentries = entries_storage->size();

    co_return co_await run_fs<std::vector<fs::Dirent>>(
        [&](uv_fs_t &req, uv_fs_cb cb) { return uv::fs_readdir(loop, req, *static_cast<uv_dir_t *>(dir.native_handle()), cb); },
        [entries_storage](uv_fs_t &req) {
            std::vector<fs::Dirent> out;
            auto *d = static_cast<uv_dir_t *>(req.ptr);
            if (d == nullptr) {
                return out;
            }

            for (u32 i = 0; i < req.result; ++i) {
                auto &ent = d->dirents[i];
                fs::Dirent de;
                if (ent.name) {
                    de.name = ent.name;
                }
                de.kind = map_dirent(ent.type);
                out.push_back(std::move(de));
            }
            return out;
        },
        loop);
}

template <typename Fn, typename Map> static auto run_sync_fs(Fn &&fn, Map &&map) {
    uv_fs_t req{};
    i32 r = fn(req);
    uv::fs_req_cleanup(req);
    return map(r);
}

template <typename Fn> static Error run_sync_fs(Fn &&fn) {
    return run_sync_fs(std::forward<Fn>(fn), [](i32 r) -> Error {
        if (r < 0) {
            return uv::status_to_error(r);
        }
        return {};
    });
}

Result<i32> fs::sync::open(std::string_view path, i32 flags, i32 mode) {
    std::string p(path);
    return run_sync_fs([&](uv_fs_t &req) { return uv::fs_open_sync(req, p.c_str(), flags, mode); },
                       [](i32 r) -> Result<i32> {
                           if (r < 0) {
                               return outcome_error(uv::status_to_error(r));
                           }
                           return r;
                       });
}

Result<usize> fs::sync::read(i32 fd, std::span<char> buf, i64 offset) {
    uv_buf_t uv_buf = uv_buf_init(buf.data(), static_cast<u32>(buf.size()));
    return run_sync_fs([&](uv_fs_t &req) { return uv::fs_read_sync(req, fd, &uv_buf, 1, offset); },
                       [](i32 r) -> Result<usize> {
                           if (r < 0) {
                               return outcome_error(uv::status_to_error(r));
                           }
                           return static_cast<usize>(r);
                       });
}

Result<usize> fs::sync::write(i32 fd, std::span<const char> buf, i64 offset) {
    uv_buf_t uv_buf = uv_buf_init(const_cast<char *>(buf.data()), static_cast<u32>(buf.size()));
    return run_sync_fs([&](uv_fs_t &req) { return uv::fs_write_sync(req, fd, &uv_buf, 1, offset); },
                       [](i32 r) -> Result<usize> {
                           if (r < 0) {
                               return outcome_error(uv::status_to_error(r));
                           }
                           return static_cast<usize>(r);
                       });
}

Error fs::sync::close(i32 fd) {
    return run_sync_fs([&](uv_fs_t &req) { return uv::fs_close_sync(req, fd); });
}

Result<std::string> fs::sync::read_to_string(std::string_view path) {
    auto fd = open(path, UV_FS_O_RDONLY);
    if (!fd) {
        return outcome_error(fd.error());
    }

    std::string content;
    char buf[4096];
    while (true) {
        auto n = read(*fd, std::span<char>(buf, sizeof(buf)));
        if (!n) {
            close(*fd);
            return outcome_error(n.error());
        }
        if (*n == 0) {
            break;
        }
        content.append(buf, *n);
    }

    close(*fd);
    return content;
}

} // namespace lighter
