#pragma once

#include <optional>
#include <string>
#include <vector>

#include <liminal/provider/provider.h>

namespace liminal::model {

/// One selectable model. Provider identity is routing metadata rather than a
/// separate user selection.
struct Entry {
    std::string provider;
    std::string id;
    std::string name;
    std::vector<std::string> reasoning_efforts;
    std::optional<std::string> default_reasoning_effort;
};

struct Choice {
    provider::Provider handle;
    Entry entry;
    std::optional<std::string> reasoning_effort;
};

} // namespace liminal::model
