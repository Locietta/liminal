#include "default_instructions.h"

#include <cstddef>
#include <string>

namespace liminal {

namespace {

constexpr unsigned char k_runtime_prompt[] = {
#include "runtime-tools.md.h"
};

constexpr unsigned char k_application_prompt[] = {
#include "default-agent.md.h"
};

static_assert(k_runtime_prompt[sizeof(k_runtime_prompt) - 1] == 0);
static_assert(k_application_prompt[sizeof(k_application_prompt) - 1] == 0);

template <std::size_t Size>
std::string embedded_prompt(const unsigned char (&content)[Size]) {
    static_assert(Size > 0);
    return {reinterpret_cast<const char *>(content), Size - 1};
}

} // namespace

context::InstructionSource default_runtime_instruction() {
    return {
        .authority = context::InstructionAuthority::RUNTIME,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:runtime-tools",
        .content = embedded_prompt(k_runtime_prompt),
    };
}

context::InstructionSource default_application_instruction() {
    return {
        .authority = context::InstructionAuthority::APPLICATION,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:default-agent",
        .content = embedded_prompt(k_application_prompt),
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
