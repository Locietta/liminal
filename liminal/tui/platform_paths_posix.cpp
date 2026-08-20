#include "platform_paths.h"

#ifndef _WIN32

#include <cstdlib>

#include <lighter/encoding/utf8.h>

namespace liminal::tui {

Result<std::string> native_path_utf8(const std::filesystem::path &path) { return lighter::encoding::utf8::sanitize(path.native()); }

Result<std::optional<std::string>> user_home_directory_utf8() {
    const auto *home = std::getenv("HOME");
    if (!home || *home == '\0') return std::nullopt;
    return std::optional(lighter::encoding::utf8::sanitize(home));
}

} // namespace liminal::tui

#endif
