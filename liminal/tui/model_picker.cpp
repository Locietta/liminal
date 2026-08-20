#include "model_picker.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace liminal::tui {

namespace {

std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    for (auto &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

} // namespace

std::vector<CompactPickerItem> model_picker_items(std::span<const model::Entry> entries, std::string_view current_provider,
                                                  std::string_view current_id, const std::optional<std::string> &current_effort) {
    // A selector suffix and the reasoning effort it resolves to. Entries with
    // declared efforts get an explicit @off row because their bare selector
    // resolves to the default effort, not to "no effort".
    struct Variant {
        std::optional<std::string> suffix;
        std::optional<std::string> effective;
    };

    std::vector<CompactPickerItem> items;
    for (const auto &entry : entries) {
        std::vector<Variant> variants;
        if (entry.reasoning_efforts.empty()) {
            variants.push_back({.suffix = std::nullopt, .effective = std::nullopt});
        } else {
            for (const auto &effort : entry.reasoning_efforts) variants.push_back({.suffix = effort, .effective = effort});
            variants.push_back({.suffix = "off", .effective = std::nullopt});
        }
        for (const auto &variant : variants) {
            auto selector = entry.provider + "/" + entry.id;
            if (variant.suffix) selector += "@" + *variant.suffix;
            CompactPickerItem item{.id = selector, .primary = selector};
            if (!entry.name.empty() && entry.name != entry.id) item.description = entry.name;
            item.current = entry.provider == current_provider && entry.id == current_id && variant.effective == current_effort;
            item.haystacks.push_back(ascii_lower(entry.provider));
            item.haystacks.push_back(ascii_lower(entry.id));
            if (!entry.name.empty()) item.haystacks.push_back(ascii_lower(entry.name));
            if (variant.suffix) item.haystacks.push_back(ascii_lower(*variant.suffix));
            item.haystacks.push_back(ascii_lower(selector));
            items.push_back(std::move(item));
        }
    }
    return items;
}

} // namespace liminal::tui
