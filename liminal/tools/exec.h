#pragma once

#include <array>
#include <filesystem>

#include "tools.h"

namespace liminal {

struct ExecSessionManager;

ExecSessionManagerPtr make_exec_session_manager(std::filesystem::path working_directory);
std::array<ToolRegistration, 2> make_exec_tools(ExecSessionManager &sessions);

} // namespace liminal
