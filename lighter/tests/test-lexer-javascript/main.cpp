#include <algorithm>
#include <iostream>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/document.h>
#include <lighter/lexer/language/javascript.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role) {
    usize position = 0;
    while ((position = document.source.find(token, position)) != std::string::npos) {
        const bool left_boundary =
            !ascii_identifier_start(token.front()) || position == 0 || !ascii_identifier_continue(document.source[position - 1]);
        const usize token_end = position + token.size();
        const bool right_boundary = !ascii_identifier_continue(token.back()) || token_end == document.source.size() ||
                                    !ascii_identifier_continue(document.source[token_end]);
        if (left_boundary && right_boundary) break;
        ++position;
    }
    if (position == std::string::npos) {
        std::cerr << "missing token: " << token << '\n';
        return false;
    }
    for (usize offset = position; offset < position + token.size(); ++offset) {
        const TokenRole actual = role_for_style<JavaScriptLexer::Style>(document.styles[offset]);
        if (actual != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got " << static_cast<int>(actual)
                      << '\n';
            return false;
        }
    }
    return true;
}

void lex(JavaScriptLexer lexer, Document &document, LexRange range) {
    auto lex_context = context(document, range);
    lexer.lex(lex_context);
}

bool test_typescript_semantics() {
    Document document;
    assign(document,
           "import Widget from \"./widget.js\";\n@sealed\nclass Controller {\n  render(value: number): string {\n    const pattern = "
           "/foo\\d+/gi;\n    return this.service.format(`value=${value}`);\n  }\n}\n");
    lex(JavaScriptLexer{.dialect = JavaScriptDialect::TYPESCRIPT}, document, {.begin = 0, .end = document.source.size()});

    return has_role(document, "import", TokenRole::KEYWORD) && has_role(document, "Widget", TokenRole::MODULE) &&
           has_role(document, "sealed", TokenRole::ATTRIBUTE) && has_role(document, "class", TokenRole::KEYWORD) &&
           has_role(document, "Controller", TokenRole::TYPE) && has_role(document, "render", TokenRole::FUNCTION) &&
           has_role(document, "number", TokenRole::TYPE) && has_role(document, "string", TokenRole::TYPE) &&
           has_role(document, "foo", TokenRole::STRING) && has_role(document, "\\d", TokenRole::ESCAPE) &&
           has_role(document, "service", TokenRole::PROPERTY) && has_role(document, "format", TokenRole::FUNCTION) &&
           has_role(document, "${", TokenRole::OPERATOR);
}

bool test_streamed_template_expression() {
    Document document;
    assign(document, "const text = `first ${\n");
    lex(JavaScriptLexer{}, document, {.begin = 0, .end = document.source.size()});

    const LexRange dirty = append(document, "compute(1)} second\n`;\n");
    lex(JavaScriptLexer{}, document, dirty);
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "compute", TokenRole::FUNCTION) &&
           has_role(document, "1", TokenRole::NUMBER) && has_role(document, "second", TokenRole::STRING);
}

bool test_documentation_comment() {
    Document document;
    assign(document, "/** @param value */\nfunction parse(value) { return value; }\n");
    lex(JavaScriptLexer{}, document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "@param", TokenRole::ATTRIBUTE) && has_role(document, "function", TokenRole::KEYWORD) &&
           has_role(document, "parse", TokenRole::FUNCTION) && has_role(document, "return", TokenRole::KEYWORD);
}

bool test_tsx_tags() {
    Document document;
    assign(document, "const view = <Panel title=\"status\" data-id={value} />;\n");
    lex(JavaScriptLexer{.dialect = JavaScriptDialect::TSX}, document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "Panel", TokenRole::TYPE) && has_role(document, "title", TokenRole::PROPERTY) &&
           has_role(document, "status", TokenRole::STRING) && has_role(document, "data-id", TokenRole::PROPERTY);
}

bool test_actionscript() {
    Document document;
    assign(document, "package demo { public class Widget implements Display { override function draw():void {} } }\n");
    lex(JavaScriptLexer{.dialect = JavaScriptDialect::ACTIONSCRIPT}, document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "package", TokenRole::KEYWORD) && has_role(document, "demo", TokenRole::MODULE) &&
           has_role(document, "Widget", TokenRole::TYPE) && has_role(document, "implements", TokenRole::KEYWORD) &&
           has_role(document, "draw", TokenRole::FUNCTION);
}

} // namespace

int main() {
    return test_typescript_semantics() && test_streamed_template_expression() && test_documentation_comment() && test_tsx_tags() &&
                   test_actionscript() ?
               0 :
               1;
}
