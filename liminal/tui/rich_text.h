#pragma once

#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

namespace liminal::tui {

using namespace lighter::types;

struct StyledRow {
    usize source_offset = 0;
    std::vector<StyledSpan> spans;
};

/// Projects the supported Markdown and unified-diff subset into styled,
/// terminal-width rows. The source remains untouched and malformed markup is
/// emitted literally.
std::vector<StyledRow> layout_rich_text(std::string_view source, i32 columns);

} // namespace liminal::tui
