#include "default_instructions.h"

namespace liminal {

context::InstructionSource default_runtime_instruction() {
    return {
        .authority = context::InstructionAuthority::RUNTIME,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:runtime-tools",
        .content =
            "You are Liminal, a coding and general-purpose assistant.\n"
            "For workspace discovery, prefer `rg` for text search and `rg --files` for file lists. Use the installed uutils "
            "commands for other routine filesystem and text operations; assume ripgrep and uutils are available on Windows and Linux.\n"
            "Use `apply_patch` for manual file edits instead of rewriting files through the shell. Use hosted web tools when a task "
            "needs current or external information, and cite the sources you rely on.",
    };
}

context::InstructionSource default_application_instruction() {
    return {
        .authority = context::InstructionAuthority::APPLICATION,
        .trust = context::InstructionTrust::PLATFORM,
        .origin = "builtin:default-agent",
        .content = "Be helpful, direct, and careful.",
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
