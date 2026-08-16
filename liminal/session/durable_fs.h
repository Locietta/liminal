#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <liminal/error.h>

namespace liminal::session::detail {

Result<void> durable_replace_file(const std::filesystem::path &path, std::string_view contents);
Result<void> durable_remove_file(const std::filesystem::path &path);
Result<void> rename_directory_without_replacement(const std::filesystem::path &source, const std::filesystem::path &target);
Result<void> flush_published_directory(const std::filesystem::path &target);
Result<bool> is_reparse_point(const std::filesystem::path &path);

} // namespace liminal::session::detail
