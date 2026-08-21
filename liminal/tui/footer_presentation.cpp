#include "footer_presentation.h"

#include <array>
#include <string>
#include <utility>

namespace liminal::tui {

namespace {

constexpr std::string_view k_separator = " · ";

struct FooterSegment {
    std::string text;
    Style style = Style::MUTED;
    bool visible = true;
};

std::string compact_tokens(u64 tokens) {
    constexpr u64 k_thousand = 1'000;
    constexpr u64 k_million = 1'000'000;
    if (tokens < k_thousand) return std::to_string(tokens);

    const auto divisor = tokens < k_million ? k_thousand : k_million;
    const auto suffix = tokens < k_million ? 'K' : 'M';
    auto whole = tokens / divisor;
    auto hundredths = ((tokens % divisor) * 100 + divisor / 2) / divisor;
    if (hundredths == 100) {
        ++whole;
        hundredths = 0;
    }

    auto result = std::to_string(whole);
    if (hundredths != 0) {
        result += '.';
        if (hundredths < 10) result += '0';
        result += std::to_string(hundredths);
        while (result.ends_with('0')) result.pop_back();
    }
    result += suffix;
    return result;
}

std::string truncate_cells(std::string_view text, i32 columns) {
    if (columns <= 0) return {};
    if (text_width(text) <= columns) return std::string(text);

    constexpr std::string_view k_ellipsis = "…";
    if (columns == 1) return std::string(k_ellipsis);

    std::string result;
    i32 used = 0;
    usize offset = 0;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        if (used + grapheme.width > columns - 1) break;
        result.append(text.substr(grapheme.offset, grapheme.size));
        used += grapheme.width;
        offset += grapheme.size;
    }
    result += k_ellipsis;
    return result;
}

i32 projected_width(const std::array<FooterSegment, 4> &segments) {
    i32 width = 0;
    bool first = true;
    for (const auto &segment : segments) {
        if (!segment.visible) continue;
        if (!first) width += text_width(k_separator);
        width += text_width(segment.text);
        first = false;
    }
    return width;
}

} // namespace

std::vector<StyledSpan> present_footer(std::string_view model, const std::optional<std::string> &effort, const SessionFooter &footer,
                                       i32 columns) {
    auto model_selection = std::string(model);
    if (effort) model_selection += " " + *effort;
    const auto context =
        footer.context_left_percent ? "Context " + std::to_string(*footer.context_left_percent) + "% left" : std::string("Context n/a");

    std::array segments{
        FooterSegment{.text = std::move(model_selection), .style = Style::FOOTER_MODEL},
        FooterSegment{.text = context, .style = Style::FOOTER_CONTEXT},
        FooterSegment{.text = compact_tokens(footer.tokens_used) + " used", .style = Style::FOOTER_TOKENS},
        FooterSegment{
            .text = footer.provider_limits.value_or(std::string{}),
            .style = Style::MUTED,
            .visible = footer.provider_limits.has_value() && !footer.provider_limits->empty(),
        },
    };

    // Whole ordinary segments degrade in the design-specified order. Exact
    // fits are retained because only strictly oversized projections degrade.
    constexpr usize k_removal_order[] = {2, 1, 3};
    for (const auto index : k_removal_order) {
        if (projected_width(segments) <= columns) break;
        segments[index].visible = false;
    }
    if (projected_width(segments) > columns) segments[0].text = truncate_cells(segments[0].text, columns);

    std::vector<StyledSpan> result;
    bool first = true;
    for (const auto &segment : segments) {
        if (!segment.visible) continue;
        if (!first) result.push_back({.text = std::string(k_separator), .style = Style::MUTED});
        result.push_back({.text = segment.text, .style = segment.style});
        first = false;
    }
    return result;
}

} // namespace liminal::tui
