#include "default_instructions.h"

#include <cstddef>
#include <span>
#include <string>

namespace liminal {

namespace {

extern "C" {
extern const std::byte _binary_runtime_tools_md_start[];
extern const std::byte _binary_runtime_tools_md_end[];
extern const std::byte _binary_default_agent_md_start[];
extern const std::byte _binary_default_agent_md_end[];
}

std::string embedded_prompt(const std::byte *begin, const std::byte *end) {
    const auto content = std::span(begin, end);
    return {reinterpret_cast<const char *>(content.data()), content.size()};
}

} // namespace

context::InstructionSource default_runtime_instruction() {
    return {
        .authority = context::InstructionAuthority::RUNTIME,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:runtime-tools",
        .content = embedded_prompt(_binary_runtime_tools_md_start, _binary_runtime_tools_md_end),
    };
}

context::InstructionSource default_application_instruction() {
    return {
        .authority = context::InstructionAuthority::APPLICATION,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:default-agent",
        .content = embedded_prompt(_binary_default_agent_md_start, _binary_default_agent_md_end),
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
