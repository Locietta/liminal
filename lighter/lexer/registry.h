#pragma once

#include <optional>
#include <string_view>

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

/// Resolves a Markdown fence language name case-insensitively. Unknown and
/// empty names deliberately return no lexer so the renderer can use its plain
/// code style.
[[nodiscard]] std::optional<Lexer> lexer_for_language(std::string_view name);

} // namespace lighter::lexer
