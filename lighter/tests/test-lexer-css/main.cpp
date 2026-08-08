#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/css.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role) {
    const usize position = document.source.find(token);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<CssLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, LexRange range) {
    auto lex_context = context(document, range);
    CssLexer{}.lex(lex_context);
}

bool test_css() {
    Document document;
    assign(
        document,
        "@media screen {\n  #app.card:hover {\n    --accent: #ff00aa;\n    color: var(--accent);\n    width: calc(100% - 2rem);\n  }\n}\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "@media", TokenRole::PREPROCESSOR) && has_role(document, "#app", TokenRole::CONSTANT) &&
           has_role(document, ".card", TokenRole::ATTRIBUTE) && has_role(document, ":hover", TokenRole::FUNCTION) &&
           has_role(document, "--accent", TokenRole::PROPERTY) && has_role(document, "color", TokenRole::PROPERTY) &&
           has_role(document, "var", TokenRole::FUNCTION) && has_role(document, "100%", TokenRole::NUMBER);
}

bool test_streamed_comment() {
    Document document;
    assign(document, "/* first\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "second */\nbody { display: block; }\n");
    lex(document, dirty);
    return has_role(document, "first", TokenRole::COMMENT) && has_role(document, "second", TokenRole::COMMENT) &&
           has_role(document, "body", TokenRole::TYPE) && has_role(document, "display", TokenRole::PROPERTY) &&
           has_role(document, "block", TokenRole::KEYWORD);
}

} // namespace

int main() { return test_css() && test_streamed_comment() ? 0 : 1; }
