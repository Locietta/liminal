#pragma once

#include <string>
#include <string_view>

#include <lighter/encoding/utf8.h>

#include <lighter/types.hpp>

namespace liminal {

inline std::string bounded_utf8(std::string_view text, lighter::types::usize maximum_bytes) {
    auto result = lighter::encoding::utf8::sanitize(text);
    if (result.size() <= maximum_bytes) return result;
    const auto complete = lighter::encoding::utf8::complete_prefix_len(std::string_view(result).substr(0, maximum_bytes));
    result.resize(complete);
    return result;
}

} // namespace liminal
