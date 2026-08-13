#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <proxy/proxy.h>

#include <liminal/context/context.h>
#include <liminal/error.h>

namespace liminal::context {

PRO_DEF_MEM_DISPATCH(CanonicalizeInstructionPathDispatch, canonicalize_instruction_path);
PRO_DEF_MEM_DISPATCH(ReadInstructionFileDispatch, read_instruction_file);

struct InstructionFilesFacade
    : pro::facade_builder::add_convention<
          CanonicalizeInstructionPathDispatch, Result<std::filesystem::path>(const std::filesystem::path &) const
      >::add_convention<ReadInstructionFileDispatch, Result<std::string>(const std::filesystem::path &) const>::build {};

using InstructionFiles = pro::proxy<InstructionFilesFacade>;

/// The host-filesystem implementation of the project-instruction boundary.
struct LocalInstructionFiles {
    Result<std::filesystem::path> canonicalize_instruction_path(const std::filesystem::path &path) const;
    Result<std::string> read_instruction_file(const std::filesystem::path &path) const;
};

/// Finds the nearest enclosing Git repository root. Outside a repository,
/// returns the canonical invocation directory unchanged.
Result<std::filesystem::path> discover_project_root(const std::filesystem::path &working_directory);

/// Resolves trusted project instructions from the project root to the
/// active directory. Both paths are canonicalized through `files`, and the
/// active directory must remain inside the project boundary.
struct ProjectInstructionResolver {
    Result<std::vector<InstructionSource>> resolve(const std::filesystem::path &project_root, const std::filesystem::path &active_directory,
                                                   const InstructionFiles &files) const;
};

} // namespace liminal::context
