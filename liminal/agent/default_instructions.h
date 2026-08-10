#pragma once

#include <vector>

#include <liminal/context/context.h>

namespace liminal {

context::InstructionSource default_runtime_instruction();
context::InstructionSource default_application_instruction();
std::vector<context::InstructionSource> default_agent_instructions();

} // namespace liminal
