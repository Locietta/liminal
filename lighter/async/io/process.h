#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/owned.h>

namespace lighter {

class EventLoop;

class Process {
public:
    Process() noexcept;

    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    Process(Process &&other) noexcept;
    Process &operator=(Process &&other) noexcept;

    ~Process();

    struct Self;
    Self *operator->() noexcept;

    struct ExitStatus {
        /// Exit code reported by the child.
        i64 status;

        /// Terminating Signal number if signalled, 0 otherwise.
        i32 term_signal;
    };

    struct Stdio {
        enum class Kind {
            INHERIT_STREAM,  // inherit parent's Stdio
            IGNORE_STREAM,   // discard this Stream
            FD_STREAM,       // inherit a specific file descriptor
            PIPE_STREAM_KIND // create a Pipe
        };

        /// How this Stream should be configured for the child.
        Kind type = Kind::INHERIT_STREAM;

        /// Descriptor to inherit when type == fd.
        i32 descriptor = -1;

        /// Child-readable flag when type == Pipe.
        bool readable = false; // from the child's perspective

        /// Child-writable flag when type == Pipe.
        bool writable = false; // from the child's perspective

        /// Inherit parent's descriptor (default).
        static Stdio inherit();

        /// Discard this Stream for the child.
        static Stdio ignore();

        /// Duplicate the given descriptor into the child.
        static Stdio from_fd(i32 fd);

        /// Create a Pipe; flags are from the child's perspective.
        static Stdio pipe(bool readable, bool writable);
    };

    struct CreationOptions {
        /// Detach the child from the parent Process group/session.
        bool detached = false;

        /// Hide the Console window (Windows).
        bool windows_hide = false;

        /// Hide the Console window specifically (Windows).
        bool windows_hide_console = false;

        /// Hide GUI window (Windows).
        bool windows_hide_gui = false;

        /// Disable argument quoting/escaping (Windows).
        bool windows_verbatim_arguments = false;

        /// Use exact file path for image name (Windows).
        bool windows_file_path_exact_name = false;
    };

    struct Options {
        /// Executable path.
        std::string file;

        /// argv (including argv[0]). If empty, defaults to `file`.
        std::vector<std::string> args;

        /// Environment variables in `KEY=VALUE` form; empty means inherit.
        std::vector<std::string> env;

        /// Working directory; empty means inherit.
        std::string cwd;

        /// Process creation Options (platform-specific Options may be ignored).
        CreationOptions creation;

        /// Stdio config for stdin/stdout/stderr.
        std::array<Stdio, 3> streams = {Stdio::inherit(), Stdio::inherit(), Stdio::inherit()};
    };

    using WaitResult = Result<ExitStatus>;

    /// Launch the Process; creates pipes as requested in Options.
    struct SpawnResult;

    /// Spawn a child Process within the given loop.
    static Result<SpawnResult> spawn(const Options &opts, EventLoop &loop = EventLoop::current());

    /// Await Process termination and fetch exit status.
    Task<WaitResult> wait();

    /// Retrieve OS pid for the Process; -1 if not started.
    i32 pid() const noexcept;

    /// Send a Signal to the Process.
    Error kill(i32 signum);

private:
    explicit Process(UniqueHandle<Self> self) noexcept;

    UniqueHandle<Self> self;
};

struct Process::SpawnResult {
    Process proc;

    Pipe stdin_pipe;

    Pipe stdout_pipe;

    Pipe stderr_pipe;
};

} // namespace lighter
