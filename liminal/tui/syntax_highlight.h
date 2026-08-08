#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <lighter/lexer/document.h>
#include <lighter/lexer/lexer.h>
#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

namespace liminal::tui {

using namespace lighter::types;

/// Stateful adapter from reflected lexers to fenced Markdown code spans.
/// Unknown languages and oversized input retain the generic code style.
struct CodeHighlighter {
    explicit CodeHighlighter(std::string_view language = {});

    std::vector<StyledSpan> highlight_line(std::string_view line);
    bool supported() const noexcept { return lexer.has_value() && enabled; }

    std::optional<lighter::lexer::Lexer> lexer;
    lighter::lexer::Document document;
    bool enabled = true;
    usize total_bytes = 0;
    usize line_count = 0;
};

} // namespace liminal::tui
