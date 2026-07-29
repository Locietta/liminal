#include "process.h"

#include <contracts>
#include <csignal>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

static u32 to_uv_process_flags(const Process::CreationOptions &options) {
    u32 out = 0;
    if (options.detached) {
        out |= UV_PROCESS_DETACHED;
    }
    if (options.windows_hide) {
        out |= UV_PROCESS_WINDOWS_HIDE;
    }
    if (options.windows_hide_console) {
        out |= UV_PROCESS_WINDOWS_HIDE_CONSOLE;
    }
    if (options.windows_hide_gui) {
        out |= UV_PROCESS_WINDOWS_HIDE_GUI;
    }
    if (options.windows_verbatim_arguments) {
        out |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;
    }
    if (options.windows_file_path_exact_name) {
        out |= UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME;
    }
    return out;
}

struct Process::Self : uv::handle<Process::Self, uv_process_t>, uv::LatchedDelivery<Process::ExitStatus> {
    uv_process_t handle{};
};

namespace {

struct ProcessAwait : uv::AwaitOp<ProcessAwait> {
    using await_base = uv::AwaitOp<ProcessAwait>;
    using promise_t = Task<Process::WaitResult>::promise_type;

    // Process self used to install/remove waiter and active result pointers.
    Process::Self *self;
    // Exit status slot filled by Process exit callback.
    Process::ExitStatus result{};

    explicit ProcessAwait(Process::Self *self) : self(self) {}

    static void on_cancel(IoOp *op) {
        auto *aw = static_cast<ProcessAwait *>(op);
        if (aw && aw->self) {
            aw->state = AsyncNode::CANCELLED;
            uv::process_kill(aw->self->handle, SIGKILL);
        }
    }

    static void notify(Process::Self &self, Process::ExitStatus status) { self.deliver(status); }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }
        self->arm(*this, result);
        return this->attach(waiting.promise(), loc);
    }

    Process::WaitResult await_resume() noexcept {
        if (self) {
            self->disarm();
        }
        return result;
    }
};

} // namespace

Process::Process() noexcept = default;

Process::Process(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

Process::~Process() = default;

Process::Process(Process &&other) noexcept = default;

Process &Process::operator=(Process &&other) noexcept = default;

Process::Self *Process::operator->() noexcept { return self.get(); }

Process::Stdio Process::Stdio::inherit() { return Stdio{}; }

Process::Stdio Process::Stdio::ignore() {
    Stdio io{};
    io.type = Kind::IGNORE_STREAM;
    return io;
}

Process::Stdio Process::Stdio::from_fd(i32 fd) {
    Stdio io{};
    io.type = Kind::FD_STREAM;
    io.descriptor = fd;
    return io;
}

Process::Stdio Process::Stdio::pipe(bool readable, bool writable) {
    Stdio io{};
    io.type = Kind::PIPE_STREAM_KIND;
    io.readable = readable;
    io.writable = writable;
    return io;
}

Result<Process::SpawnResult> Process::spawn(const Options &opts, EventLoop &loop) {
    SpawnResult out{Process(Self::make()), {}, {}, {}};

    std::vector<std::string> argv_storage;
    if (opts.args.empty()) {
        argv_storage.push_back(opts.file);
    } else {
        argv_storage = opts.args;
    }

    std::vector<char *> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto &arg : argv_storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = opts.env;
    std::vector<char *> envp;
    if (!env_storage.empty()) {
        envp.reserve(env_storage.size() + 1);
        for (auto &e : env_storage) {
            envp.push_back(e.data());
        }
        envp.push_back(nullptr);
    }

    std::array<Pipe, 3> created_pipes{};
    std::array<uv_stdio_container_t, 3> stdio{};

    for (usize i = 0; i < opts.streams.size(); ++i) {
        auto &cfg = opts.streams[i];
        auto &dst = stdio[i];

        switch (cfg.type) {
            case Stdio::Kind::INHERIT_STREAM:
                dst.flags = UV_INHERIT_FD;
                dst.data.fd = static_cast<i32>(i);
                break;
            case Stdio::Kind::IGNORE_STREAM: dst.flags = UV_IGNORE; break;
            case Stdio::Kind::FD_STREAM:
                dst.flags = UV_INHERIT_FD;
                dst.data.fd = cfg.descriptor;
                break;
            case Stdio::Kind::PIPE_STREAM_KIND: {
                auto pipe = Pipe::create(Pipe::Options{}, loop);
                if (!pipe) {
                    return outcome_error(pipe.error());
                }

                dst.flags = UV_CREATE_PIPE;
                if (cfg.readable) {
                    dst.flags = static_cast<uv_stdio_flags>(dst.flags | static_cast<i32>(UV_READABLE_PIPE));
                }
                if (cfg.writable) {
                    dst.flags = static_cast<uv_stdio_flags>(dst.flags | static_cast<i32>(UV_WRITABLE_PIPE));
                }

                dst.data.stream = static_cast<uv_stream_t *>(pipe->handle());

                created_pipes[i] = std::move(*pipe);
                break;
            }
        }
    }

    uv_process_options_t uv_opts{};
    uv_opts.exit_cb = +[](uv_process_t *handle, i64 exit_status, i32 term_signal) {
        auto *self = static_cast<Process::Self *>(handle->data);
        contract_assert(self != nullptr);

        ProcessAwait::notify(*self, Process::ExitStatus{exit_status, term_signal});
    };
    uv_opts.file = opts.file.c_str();
    uv_opts.args = argv.data();
    uv_opts.stdio_count = static_cast<i32>(stdio.size());
    uv_opts.stdio = stdio.data();

    if (!envp.empty()) {
        uv_opts.env = envp.data();
    }

    if (!opts.cwd.empty()) {
        uv_opts.cwd = opts.cwd.c_str();
    }

    uv_opts.flags = to_uv_process_flags(opts.creation);

    auto *self = out.proc.self.get();
    if (self == nullptr) {
        return outcome_error(Error::k_invalid_argument);
    }

    auto &proc_handle = self->handle;
    if (auto err = uv::spawn(loop, proc_handle, uv_opts)) {
        return outcome_error(err);
    }

    out.stdin_pipe = std::move(created_pipes[0]);
    out.stdout_pipe = std::move(created_pipes[1]);
    out.stderr_pipe = std::move(created_pipes[2]);

    return out;
}

Task<Process::WaitResult> Process::wait() {
    if (!self) {
        co_return outcome_error(Error::k_invalid_argument);
    }

    if (self->has_pending()) {
        co_return self->peek_pending();
    }

    if (self->has_waiter()) {
        co_return outcome_error(Error::k_connection_already_in_progress);
    }

    co_return co_await ProcessAwait{self.get()};
}

i32 Process::pid() const noexcept {
    if (!self || !self->initialized()) {
        return -1;
    }

    return self->handle.pid;
}

Error Process::kill(i32 signum) {
    if (!self || !self->initialized()) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::process_kill(self->handle, signum)) {
        return err;
    }

    return {};
}

} // namespace lighter
