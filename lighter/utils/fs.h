#pragma once

#include <filesystem>

namespace lighter {

/// return the directory containing the current executable, returns empty path on failure
std::filesystem::path executable_directory();

/// return the path to the current executable, returns empty path on failure
std::filesystem::path executable_path();

} // namespace lighter