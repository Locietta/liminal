#pragma once

#include <string_view>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/tui/surface.h>

namespace liminal::tui {

using namespace lighter::types;

enum struct CodeLanguage : u8 {
    PLAIN,
    C_LIKE,
    RUST,
    JAVASCRIPT,
    PYTHON,
    SHELL,
    GO,
    SQL,
    DATA,
    CONFIG,
};

/// Lightweight, stateful syntax highlighter for fenced Markdown code. Unknown
/// languages and oversized input retain the generic code style.
struct CodeHighlighter {
    explicit CodeHighlighter(std::string_view language = {});

    std::vector<StyledSpan> highlight_line(std::string_view line);
    bool supported() const noexcept { return language != CodeLanguage::PLAIN && enabled; }

    CodeLanguage language = CodeLanguage::PLAIN;
    bool block_comment = false;
    char multiline_quote = 0;
    bool multiline_triple = false;
    bool enabled = true;
    usize total_bytes = 0;
    usize line_count = 0;
};

} // namespace liminal::tui
