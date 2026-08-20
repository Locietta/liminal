#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/model/model.h>
#include <liminal/tui/compact_picker.h>

namespace liminal::tui {

/// Expands catalog entries into compact-picker rows, one per selectable
/// reasoning effort. Row identities are exact catalog selectors of the form
/// provider/id[@effort], so a confirmed row maps back through Catalog::select
/// and duplicate model IDs stay disambiguated by provider.
std::vector<CompactPickerItem> model_picker_items(std::span<const model::Entry> entries, std::string_view current_provider,
                                                  std::string_view current_id, const std::optional<std::string> &current_effort);

} // namespace liminal::tui
