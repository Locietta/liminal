#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include <lighter/async/runtime/task.h>

#include "liminal/error.h"
#include "liminal/provider/auth.h"

namespace liminal::codex {

using DeviceCodeNotice = std::function_ref<void(std::string_view verification_url, std::string_view user_code)>;

std::filesystem::path default_auth_file();

Result<std::optional<provider::AuthResolver>> load_auth(const std::filesystem::path &path);

lighter::Task<void, Error> login_device(std::filesystem::path path, DeviceCodeNotice notice);

} // namespace liminal::codex
