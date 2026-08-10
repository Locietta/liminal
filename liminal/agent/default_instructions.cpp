#include "default_instructions.h"

#include <cstddef>
#include <span>
#include <string>

#include <xmake/bin2obj/default_agent_md.hpp>
#include <xmake/bin2obj/runtime_tools_md.hpp>

namespace liminal {

namespace {

std::string embedded_prompt(std::span<const std::byte> content) { return {reinterpret_cast<const char *>(content.data()), content.size()}; }

} // namespace

context::InstructionSource default_runtime_instruction() {
    return {
        .authority = context::InstructionAuthority::RUNTIME,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:runtime-tools",
        .content = embedded_prompt(xmake::runtime_tools_md),
    };
}

context::InstructionSource default_application_instruction() {
    return {
        .authority = context::InstructionAuthority::APPLICATION,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:default-agent",
        .content = embedded_prompt(xmake::default_agent_md),
    };
}

std::vector<context::InstructionSource> default_agent_instructions() {
    std::vector<context::InstructionSource> instructions;
    instructions.reserve(2);
    instructions.push_back(default_runtime_instruction());
    instructions.push_back(default_application_instruction());
    return instructions;
}

} // namespace liminal
