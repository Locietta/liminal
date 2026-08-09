#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/bash.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) {
        std::cerr << "missing token: " << token << '\n';
        return false;
    }
    for (usize offset = position; offset < position + token.size(); ++offset) {
        const TokenRole actual = role_for_style<BashLexer::Style>(document.styles[offset]);
        if (actual != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got " << static_cast<int>(actual)
                      << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, LexRange range) {
    auto lex_context = context(document, range);
    BashLexer{}.lex(lex_context);
}

bool test_semantic_styles() {
    Document document;
    assign(document, "function greet() {\n  local name=$1\n  printf \"hello %s, ${name}\\n\" \"$name\"\n}\n# comment\ngreet world\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "function", TokenRole::KEYWORD) && has_role(document, "greet", TokenRole::FUNCTION) &&
           has_role(document, "name", TokenRole::PROPERTY) && has_role(document, "$1", TokenRole::PROPERTY) &&
           has_role(document, "printf", TokenRole::FUNCTION) && has_role(document, "${name}", TokenRole::PARAMETER) &&
           has_role(document, "\\n", TokenRole::ESCAPE) && has_role(document, "# comment", TokenRole::COMMENT) &&
           has_role(document, "world", TokenRole::IDENTIFIER);
}

bool test_command_invocations_and_options() {
    Document document;
    assign(document, "git diff --check && rg -n 'needle' . --glob '*.cpp' | head -n 5\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    const usize rg = document.source.find("rg");
    const usize head = document.source.find("head");
    return has_role(document, "git", TokenRole::FUNCTION) && has_role(document, "--check", TokenRole::ATTRIBUTE) &&
           has_role(document, "rg", TokenRole::FUNCTION, rg) && has_role(document, "-n", TokenRole::ATTRIBUTE, rg) &&
           has_role(document, "--glob", TokenRole::ATTRIBUTE, rg) && has_role(document, "head", TokenRole::FUNCTION, head) &&
           has_role(document, "-n", TokenRole::ATTRIBUTE, head);
}

bool test_streamed_quotes_and_heredoc() {
    Document document;
    assign(document, "message=\"first\nsecond $USER\"\ncat <<'DONE'\nliteral $HOME\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "DONE\nprintf '%s' \"$message\"\n");
    lex(document, dirty);
    const usize heredoc = document.source.find("literal");
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "$USER", TokenRole::PROPERTY) && has_role(document, "literal $HOME", TokenRole::STRING, heredoc) &&
           has_role(document, "DONE", TokenRole::LABEL, heredoc) && has_role(document, "printf", TokenRole::FUNCTION, heredoc);
}

} // namespace

int main() { return test_semantic_styles() && test_command_invocations_and_options() && test_streamed_quotes_and_heredoc() ? 0 : 1; }
