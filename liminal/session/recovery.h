#pragma once

#include <lighter/types.hpp>

#include <liminal/session/session.h>

namespace liminal::session {

struct RecoveryResult {
    usize recovered_tasks = 0;
    usize unknown_tool_outcomes = 0;
};

RecoveryResult recover_interrupted(Session &session);

} // namespace liminal::session
