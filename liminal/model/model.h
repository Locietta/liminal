#pragma once

#include <optional>
#include <string>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/provider/provider.h>

namespace liminal::model {

inline constexpr lighter::u32 k_default_max_output_tokens = 8192;

/// One selectable model. Provider identity is routing metadata rather than a
/// separate user selection.
struct Entry {
    std::string provider;
    std::string id;
    std::string name;
    /// Total provider context window. Unknown discovered models leave this
    /// unset until explicit configuration supplies a trustworthy value.
    std::optional<lighter::u32> context_window;
    lighter::u32 max_output_tokens = k_default_max_output_tokens;
    /// Additional input headroom for tokenizer estimation and provider-owned
    /// request material.
    lighter::u32 context_safety_margin_tokens = 0;
    std::vector<std::string> reasoning_efforts;
    std::optional<std::string> default_reasoning_effort;
};

struct Choice {
    provider::Provider handle;
    Entry entry;
    std::optional<std::string> reasoning_effort;
};

} // namespace liminal::model
