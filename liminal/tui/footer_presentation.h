#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

namespace liminal::tui {

using namespace lighter::types;

/// Width-independent session metadata. Provider limits are an opaque,
/// optional display segment until an authoritative provider contract exists.
struct SessionFooter {
    std::optional<u32> context_left_percent;
    u64 tokens_used = 0;
    std::optional<std::string> provider_limits;
    bool not_saving = false;
};

/// Pure, disposable projection of the ordinary footer metadata. Critical
/// status rows are selected by SessionScreen before this projector is used.
std::vector<StyledSpan> present_footer(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer,
                                       i32 columns);

} // namespace liminal::tui
