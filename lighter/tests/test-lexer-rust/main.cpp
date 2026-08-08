#include <algorithm>
#include <iostream>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/document.h>
#include <lighter/lexer/language/rust.h>
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
        const TokenRole actual = role_for_style<RustLexer::Style>(document.styles[offset]);
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
    RustLexer{}.lex(lex_context);
}

bool test_semantic_styles() {
    Document document;
    assign(
        document,
        "use std::fmt::Debug;\n#[derive(Debug)]\n#[deprecated(note = \"test\")] struct Widget<'a> { name: &'a str }\nimpl Widget<'_> { fn "
        "new(name: &str) -> Self { println!(\"{}\", name); Self { name } } }\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "use", TokenRole::KEYWORD) && has_role(document, "std", TokenRole::MODULE) &&
           has_role(document, "derive", TokenRole::ATTRIBUTE) && has_role(document, "struct", TokenRole::KEYWORD) &&
           has_role(document, "Widget", TokenRole::TYPE) && has_role(document, "'a", TokenRole::LABEL) &&
           has_role(document, "str", TokenRole::TYPE) && has_role(document, "new", TokenRole::FUNCTION) &&
           has_role(document, "println", TokenRole::FUNCTION) && has_role(document, "\"{}\"", TokenRole::STRING);
}

bool test_nested_comment_streaming() {
    Document document;
    assign(document, "fn prior() {}\n/* outer /* inner");
    lex(document, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "\n*/ still */ fn next() {}");
    lex(document, dirty);
    return has_role(document, "still", TokenRole::COMMENT) && has_role(document, "next", TokenRole::FUNCTION);
}

bool test_raw_string_state() {
    Document document;
    assign(document, "let text = r##\"first\nsecond\"##;\nlet value = 7;\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "7", TokenRole::NUMBER);
}

} // namespace

int main() { return test_semantic_styles() && test_nested_comment_streaming() && test_raw_string_state() ? 0 : 1; }
