#pragma once

#include <array>
#include <filesystem>

#include "tools.h"

namespace liminal {

struct ShellTaskManager;

ShellTaskManagerPtr make_shell_task_manager(std::filesystem::path working_directory);
std::array<ToolRegistration, 2> make_exec_tools(ShellTaskManager &tasks);

} // namespace liminal
