#include <iostream>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/document.h>
#include <lighter/lexer/language/go.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role) {
    usize position = document.source.find(token);
    if (position == std::string::npos) {
        std::cerr << "missing token: " << token << '\n';
        return false;
    }
    for (usize offset = position; offset < position + token.size(); ++offset) {
        const TokenRole actual = role_for_style<GoLexer::Style>(document.styles[offset]);
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
    GoLexer{}.lex(lex_context);
}

bool test_semantic_styles() {
    Document document;
    assign(document, "package main\nimport \"example\"\ntype Server struct { Name string }\nfunc Render(value int) string {\n  message := "
                     "fmt.Sprintf(\"value=%d\", value)\n  return message\n}\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "package", TokenRole::KEYWORD) && has_role(document, "main", TokenRole::MODULE) &&
           has_role(document, "Server", TokenRole::TYPE) && has_role(document, "Render", TokenRole::FUNCTION) &&
           has_role(document, "value", TokenRole::PARAMETER) && has_role(document, "int", TokenRole::TYPE) &&
           has_role(document, "fmt", TokenRole::MODULE) && has_role(document, "Sprintf", TokenRole::FUNCTION) &&
           has_role(document, "%d", TokenRole::ESCAPE) && has_role(document, "return", TokenRole::KEYWORD);
}

bool test_streamed_raw_string() {
    Document document;
    assign(document, "value := `first\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "second`\nnext := 7\n");
    lex(document, dirty);
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "7", TokenRole::NUMBER);
}

} // namespace

int main() { return test_semantic_styles() && test_streamed_raw_string() ? 0 : 1; }
