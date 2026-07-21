#include "stream.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/io/awaiter.h>

namespace lighter {

namespace {

Error ensure_reading(Stream::Self *self, Stream::Self::ReadMode mode, uv_alloc_cb alloc_cb, uv_read_cb read_cb) {
    if (self == nullptr) {
        return Error::k_invalid_argument;
    }

    if (self->active_read_mode == mode) {
        return {};
    }

    if (self->active_read_mode != Stream::Self::ReadMode::NONE) {
        uv::read_stop(self->stream);
        self->active_read_mode = Stream::Self::ReadMode::NONE;
    }

    if (auto err = uv::read_start(self->stream, alloc_cb, read_cb)) {
        return err;
    }

    self->active_read_mode = mode;
    return {};
}

struct StreamReadAwait : uv::AwaitOp<StreamReadAwait> {
    using await_base = uv::AwaitOp<StreamReadAwait>;
    // Stream self used to register reader waiter and store Error status.
    Stream::Self *self;

    explicit StreamReadAwait(Stream::Self *self) : self(self) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                if (aw.self->active_read_mode != Stream::Self::ReadMode::NONE) {
                    uv::read_stop(aw.self->stream);
                    aw.self->active_read_mode = Stream::Self::ReadMode::NONE;
                }
                aw.self->reader.disarm();
            }
        });
    }

    static void on_alloc(uv_handle_t *handle, usize, uv_buf_t *buf) {
        auto s = static_cast<Stream::Self *>(handle->data);
        assert(s != nullptr && "on_alloc requires Stream state in handle->data");

        auto [dst, writable] = s->buffer.get_write_ptr();
        buf->base = dst;
        buf->len = static_cast<decltype(buf->len)>(writable);

        if (writable == 0) {
            uv::read_stop(*reinterpret_cast<uv_stream_t *>(handle));
            s->active_read_mode = Stream::Self::ReadMode::NONE;
        }
    }

    // When nread=0, it means no data was read but the Stream is still alive (e.g., EAGAIN).
    static void on_read(uv_stream_t *stream, isize nread, const uv_buf_t *) {
        auto s = static_cast<Stream::Self *>(stream->data);
        assert(s != nullptr && "on_read requires Stream state in stream->data");
        if (auto err = uv::status_to_error(nread)) {
            uv::read_stop(*stream);
            s->active_read_mode = Stream::Self::ReadMode::NONE;
            if (s->reader.has_waiter()) {
                auto *reader = s->reader.waiter;
                s->reader.mark_cancelled_if(nread);
                s->reader.disarm();
                s->error_code = err;
                reader->complete();
            }
            return;
        }

        s->buffer.advance_write(static_cast<usize>(nread));

        if (s->reader.has_waiter()) {
            auto *reader = s->reader.waiter;
            s->reader.disarm();
            s->error_code = {};
            reader->complete();
        }
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }

        // Buffered reads intentionally leave libuv reading across await boundaries so later
        // read_chunk()/read() calls can wait for more bytes without tearing the watcher down.
        // If we are already in buffered mode, there is nothing to restart. If another read style
        // was active, switch callbacks by stopping that watcher first.
        if (auto err = ensure_reading(self, Stream::Self::ReadMode::BUFFERED, on_alloc, on_read)) {
            self->error_code = err;
            return waiting;
        }
        self->reader.arm(*this);
        return this->attach(waiting.promise(), loc);
    }

    Error await_resume() noexcept { return self->error_code; }
};

struct StreamReadSomeAwait : uv::AwaitOp<StreamReadSomeAwait> {
    using await_base = uv::AwaitOp<StreamReadSomeAwait>;
    using promise_t = Task<usize, Error>::promise_type;

    // Stream self that owns the active read waiter.
    Stream::Self *self;
    // Destination buffer provided by the caller.
    std::span<char> dst;
    // Final read result observed by await_resume().
    Result<usize> out = outcome_error(Error());

    StreamReadSomeAwait(Stream::Self *self, std::span<char> buffer) : self(self), dst(buffer) {}

    static void on_cancel(IoOp *op) {
        await_base::complete_cancel(op, [](auto &aw) {
            if (aw.self) {
                if (aw.self->active_read_mode != Stream::Self::ReadMode::NONE) {
                    uv::read_stop(aw.self->stream);
                    aw.self->active_read_mode = Stream::Self::ReadMode::NONE;
                }
                aw.self->reader.disarm();
            }
        });
    }

    static void on_alloc(uv_handle_t *handle, usize, uv_buf_t *buf) {
        auto s = static_cast<Stream::Self *>(handle->data);
        assert(s != nullptr && "on_alloc requires Stream state in handle->data");

        // stop() calls uv_read_stop then disarm(), but libuv may still invoke
        // a queued on_alloc callback after the stop. Tolerate waiter == nullptr
        // by returning a zero-length buffer so the subsequent on_read sees EOF
        // or nread==0 and exits harmlessly.
        auto *aw = static_cast<StreamReadSomeAwait *>(s->reader.waiter);
        if (!aw || aw->dst.empty()) {
            buf->base = nullptr;
            buf->len = 0;
            return;
        }

        buf->base = aw->dst.data();
        buf->len = static_cast<u32>(aw->dst.size());
    }

    // When nread=0, it means no data was read but the Stream is still alive (e.g., EAGAIN).
    static void on_read(uv_stream_t *stream, isize nread, const uv_buf_t *) {
        auto s = static_cast<Stream::Self *>(stream->data);
        assert(s != nullptr && "on_read requires Stream state in stream->data");

        // stop() may have already disarmed the waiter. If a queued on_read
        // fires after stop(), there is nothing left to complete - just bail out.
        auto *aw = static_cast<StreamReadSomeAwait *>(s->reader.waiter);
        if (!aw) {
            return;
        }

        if (nread == UV_EOF) {
            aw->out = usize{0};
        } else if (auto err = uv::status_to_error(nread)) {
            aw->out = outcome_error(err);
            aw->mark_cancelled_if(nread);
        } else if (nread > 0) {
            aw->out = static_cast<usize>(nread);
        } else {
            // nread=0 with no Error means no data was read, but the Stream is still alive (e.g.,
            // EAGAIN).
            return;
        }

        uv::read_stop(*stream);
        s->active_read_mode = Stream::Self::ReadMode::NONE;
        s->reader.disarm();
        aw->complete();
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }

        self->reader.arm(*this);
        if (auto err = ensure_reading(self, Stream::Self::ReadMode::DIRECT, on_alloc, on_read)) {
            out = outcome_error(err);
            self->reader.disarm();
            return waiting;
        }

        return this->attach(waiting.promise(), loc);
    }

    Result<usize> await_resume() noexcept {
        if (self) {
            self->reader.disarm();
        }
        return std::move(out);
    }
};

struct StreamWriteAwait : uv::AwaitOp<StreamWriteAwait> {
    using promise_t = Task<void, Error>::promise_type;

    // Stream self that owns the active write waiter.
    Stream::Self *self;
    // Owns outbound bytes until libuv invokes on_write().
    std::vector<char> storage;
    // libuv write request; req.data points back to this Awaiter.
    uv_write_t req{};
    // Completion status returned from await_resume().
    Error error_code;

    StreamWriteAwait(Stream::Self *self, std::span<const char> data) : self(self), storage(data.begin(), data.end()) {}

    static void on_cancel(IoOp *op) {
        auto *aw = static_cast<StreamWriteAwait *>(op);
        if (!aw->self) {
            return;
        }
        // uv_write_t is not cancellable via uv_cancel().
        // Keep the request in-flight and wait for on_write() to retire it.
    }

    static void on_write(uv_write_t *req, i32 status) {
        auto *aw = static_cast<StreamWriteAwait *>(req->data);
        assert(aw != nullptr && "on_write requires Awaiter in req->data");
        assert(aw->self != nullptr && "on_write requires Stream state");

        aw->mark_cancelled_if(status);

        if (auto err = uv::status_to_error(status)) {
            aw->error_code = err;
        }

        if (aw->self->writer.has_waiter()) {
            auto *w = aw->self->writer.waiter;
            aw->self->writer.disarm();
            w->complete();
        }
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_t> waiting,
                                          std::source_location loc = std::source_location::current()) noexcept {
        if (!self) {
            return waiting;
        }

        self->writer.arm(*this);
        req.data = this;

        uv_buf_t buf = uv::buf_init(storage.empty() ? nullptr : storage.data(), static_cast<u32>(storage.size()));
        if (auto err = uv::write(req, self->stream, std::span<const uv_buf_t>{&buf, 1}, on_write)) {
            error_code = err;
            self->writer.disarm();
            return waiting;
        }

        return this->attach(waiting.promise(), loc);
    }

    Error await_resume() noexcept {
        if (self) {
            self->writer.disarm();
        }
        return this->error_code;
    }
};

} // namespace

Stream::Stream() noexcept = default;

Stream::Stream(Stream &&other) noexcept = default;

Stream &Stream::operator=(Stream &&other) noexcept = default;

Stream::~Stream() = default;

Stream::Self *Stream::operator->() noexcept { return self.get(); }

void *Stream::handle() noexcept { return self ? &self->stream : nullptr; }

const void *Stream::handle() const noexcept { return self ? &self->stream : nullptr; }

HandleType guess_handle(i32 fd) {
    switch (uv::guess_handle(fd)) {
        case UV_FILE: return HandleType::FILE;
        case UV_TTY: return HandleType::TTY;
        case UV_NAMED_PIPE: return HandleType::PIPE;
        case UV_TCP: return HandleType::TCP;
        case UV_UDP: return HandleType::UDP;
        default: return HandleType::UNKNOWN;
    }
}

Task<std::string, Error> Stream::read() {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->buffer.readable_bytes() == 0) {
        if (auto err = co_await StreamReadAwait{self.get()}) {
            co_await fail(err);
        }
    }

    std::string out;
    out.resize(self->buffer.readable_bytes());
    self->buffer.read(out.data(), out.size());
    co_return out;
}

Task<usize, Error> Stream::read_some(std::span<char> dst) {
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (dst.empty()) {
        co_return usize{0};
    }

    if (self->buffer.readable_bytes() != 0) {
        const auto available = self->buffer.readable_bytes();
        const auto to_read = std::min(dst.size(), available);
        self->buffer.read(dst.data(), to_read);
        co_return to_read;
    }

    co_return co_await StreamReadSomeAwait{self.get(), dst};
}

Task<Stream::Chunk, Error> Stream::read_chunk() {
    Chunk out{};
    if (!self) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->buffer.readable_bytes() == 0) {
        if (auto err = co_await StreamReadAwait{self.get()}) {
            co_await fail(err);
        }
    }

    auto [ptr, len] = self->buffer.get_read_ptr();
    out = std::span<const char>(ptr, len);
    co_return out;
}

void Stream::consume(usize n) {
    if (!self) {
        return;
    }

    self->buffer.advance_read(n);
}

void Stream::stop() {
    // Runtime guard: match all other public methods. assert alone compiles
    // out in NDEBUG builds, leaving UB on default-constructed/moved-from streams.
    if (!self || !self->initialized()) {
        return;
    }

    // Capture the mode before resetting - we need it to pick the right
    // Error-delivery path for the pending Awaiter below.
    auto mode = self->active_read_mode;

    if (mode != Self::ReadMode::NONE) {
        uv::read_stop(self->stream);
        self->active_read_mode = Self::ReadMode::NONE;
    }

    if (self->reader.has_waiter()) {
        auto *reader = self->reader.waiter;
        self->reader.disarm();

        // For buffered reads (StreamReadAwait), await_resume() returns
        // self->error_code, so setting it here is sufficient.
        self->error_code = Error::k_operation_aborted;

        // For direct reads (StreamReadSomeAwait), await_resume() returns
        // aw->out instead of self->error_code. Propagate the Error there too
        // so the caller observes operation_aborted rather than a default Error.
        if (mode == Self::ReadMode::DIRECT) {
            static_cast<StreamReadSomeAwait *>(reader)->out = outcome_error(Error::k_operation_aborted);
        }

        reader->complete();
    }
}

Task<void, Error> Stream::write(std::span<const char> data) {
    if (!self || !self->initialized() || data.empty()) {
        co_await fail(Error::k_invalid_argument);
    }

    if (self->writer.has_waiter()) {
        assert(false && "Stream::write supports a single writer at a time");
        co_await fail(Error::k_invalid_argument);
    }

    if (auto err = co_await StreamWriteAwait{self.get(), data}) {
        co_await fail(std::move(err));
    }
}

Result<usize> Stream::try_write(std::span<const char> data) {
    if (!self || !self->initialized()) {
        return outcome_error(Error::k_invalid_argument);
    }

    if (data.empty()) {
        return usize{0};
    }

    if (data.size() > static_cast<usize>(std::numeric_limits<u32>::max())) {
        return outcome_error(Error::k_value_too_large_for_defined_data_type);
    }

    uv_buf_t buf = uv::buf_init(const_cast<char *>(data.data()), static_cast<u32>(data.size()));
    auto res = uv::try_write(self->stream, std::span<const uv_buf_t>{&buf, 1});
    if (!res) {
        return outcome_error(res.error());
    }

    return *res;
}

bool Stream::readable() const noexcept {
    if (!self || !self->initialized()) {
        return false;
    }

    return uv::is_readable(self->stream);
}

bool Stream::writable() const noexcept {
    if (!self || !self->initialized()) {
        return false;
    }

    return uv::is_writable(self->stream);
}

Error Stream::set_blocking(bool enabled) {
    if (!self || !self->initialized()) {
        return Error::k_invalid_argument;
    }

    if (auto err = uv::stream_set_blocking(self->stream, enabled)) {
        return err;
    }

    return {};
}

Stream::Stream(UniqueHandle<Self> self) noexcept : self(std::move(self)) {}

} // namespace lighter
