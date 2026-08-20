#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <liminal/error.h>

namespace liminal::tui {

/// Converts a host-native path to valid UTF-8 without changing its separator
/// convention.
Result<std::string> native_path_utf8(const std::filesystem::path &path);

/// Resolves the host user's home directory as valid UTF-8 when configured.
Result<std::optional<std::string>> user_home_directory_utf8();

} // namespace liminal::tui
