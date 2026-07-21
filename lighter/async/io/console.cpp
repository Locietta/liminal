#include "stream.h"

#include <lighter/scalar_types.hpp>
#include <lighter/async/io/awaiter.h>
#include <lighter/async/io/loop.h>

namespace lighter {

Result<Console> Console::open(i32 fd, Console::Options opts, EventLoop &loop) {
    auto self = Self::make();
    if (auto err = uv::tty_init(loop, self->tty, fd, opts.readable)) {
        return outcome_error(err);
    }

    return Console(std::move(self));
}

Error Console::set_mode(Mode value) {
    if (!self || !self->initialized()) {
        return Error::k_invalid_argument;
    }

    uv_tty_mode_t uv_mode = UV_TTY_MODE_NORMAL;
    switch (value) {
        case Mode::NORMAL: uv_mode = UV_TTY_MODE_NORMAL; break;
        case Mode::RAW: uv_mode = UV_TTY_MODE_RAW; break;
        case Mode::IO: uv_mode = UV_TTY_MODE_IO; break;
        case Mode::RAW_VT: uv_mode = UV_TTY_MODE_RAW_VT; break;
    }

    if (auto err = uv::tty_set_mode(self->tty, uv_mode)) {
        return err;
    }

    return {};
}

Error Console::reset_mode() {
    if (auto err = uv::tty_reset_mode()) {
        return err;
    }
    return {};
}

Result<Console::Winsize> Console::get_winsize() const {
    if (!self || !self->initialized()) {
        return outcome_error(Error::k_invalid_argument);
    }

    auto out = uv::tty_get_winsize(self->tty);
    if (!out) {
        return outcome_error(out.error());
    }

    return Winsize{out->width, out->height};
}

void Console::set_vterm_state(VtermState state) {
    auto uv_state = state == VtermState::SUPPORTED ? UV_TTY_SUPPORTED : UV_TTY_UNSUPPORTED;
    uv::tty_set_vterm_state(uv_state);
}

Result<Console::VtermState> Console::get_vterm_state() {
    auto out = uv::tty_get_vterm_state();
    if (!out) {
        return outcome_error(out.error());
    }

    return *out == UV_TTY_SUPPORTED ? VtermState::SUPPORTED : VtermState::UNSUPPORTED;
}

Console::Console(UniqueHandle<Self> self) noexcept : Stream(std::move(self)) {}

} // namespace lighter
