#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/markup.h>
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
        const TokenRole actual = role_for_style<MarkupLexer::Style>(document.styles[offset]);
        if (actual != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got " << static_cast<int>(actual)
                      << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, MarkupDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    MarkupLexer{.dialect = dialect}.lex(lex_context);
}

bool test_html_and_xml() {
    Document html;
    assign(html, "<!doctype html>\n<!-- first\nsecond -->\n<main class=\"app\" data-id='7'>hello &amp;</main>\n");
    lex(html, MarkupDialect::HTML, {.begin = 0, .end = html.source.size()});

    Document xml;
    assign(xml, "<?xml version=\"1.0\"?>\n<root><![CDATA[first\nsecond]]><item id=\"1\"/></root>\n");
    lex(xml, MarkupDialect::XML, {.begin = 0, .end = xml.source.size()});
    return has_role(html, "<!doctype html>", TokenRole::PREPROCESSOR) && has_role(html, "first", TokenRole::COMMENT) &&
           has_role(html, "second", TokenRole::COMMENT) && has_role(html, "main", TokenRole::TYPE) &&
           has_role(html, "class", TokenRole::PROPERTY) && has_role(html, "\"app\"", TokenRole::STRING) &&
           has_role(html, "&amp;", TokenRole::ESCAPE) && has_role(xml, "CDATA", TokenRole::DOCUMENTATION) &&
           has_role(xml, "item", TokenRole::TYPE) && has_role(xml, "id", TokenRole::PROPERTY);
}

bool test_streamed_markdown_fence() {
    Document markdown;
    assign(markdown, "# Heading\nA [link](https://example.com) and `code`.\n```cpp\nint main() {\n");
    lex(markdown, MarkupDialect::MARKDOWN, {.begin = 0, .end = markdown.source.size()});
    const LexRange dirty = append(markdown, "}\n```\nAfter.\n");
    lex(markdown, MarkupDialect::MARKDOWN, dirty);
    const usize fence = markdown.source.find("```cpp");
    return has_role(markdown, "# Heading", TokenRole::MODULE) && has_role(markdown, "[link]", TokenRole::STRING) &&
           has_role(markdown, "(https://example.com)", TokenRole::MODULE) && has_role(markdown, "`code`", TokenRole::STRING) &&
           has_role(markdown, "```", TokenRole::LABEL, fence) && has_role(markdown, "int main()", TokenRole::STRING, fence) &&
           has_role(markdown, "```", TokenRole::LABEL, markdown.source.find("int main()"));
}

} // namespace

int main() { return test_html_and_xml() && test_streamed_markdown_fence() ? 0 : 1; }
