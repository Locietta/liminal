#pragma once

#include <filesystem>
#include <memory>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

struct SessionLease {
    struct State;

    ~SessionLease();

    SessionLease(SessionLease &&) noexcept;
    SessionLease &operator=(SessionLease &&) noexcept;
    SessionLease(const SessionLease &) = delete;
    SessionLease &operator=(const SessionLease &) = delete;

private:
    friend Result<SessionLease> acquire_session_lease(const std::filesystem::path &state_root, SessionId id);
    explicit SessionLease(std::shared_ptr<State> state);
    std::shared_ptr<State> state;
};

Result<SessionLease> acquire_session_lease(const std::filesystem::path &state_root, SessionId id);

} // namespace liminal::session
