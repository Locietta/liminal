#include <algorithm>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/cpp.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role) {
    const usize position = document.source.find(token);
    if (position == std::string::npos) return false;
    return std::ranges::all_of(document.styles.begin() + static_cast<isize>(position),
                               document.styles.begin() + static_cast<isize>(position + token.size()),
                               [role](u8 style) { return role_for_style<CppLexer::Style>(style) == role; });
}

void lex(CppLexer lexer, Document &document, LexRange range) {
    auto lex_context = context(document, range);
    lexer.lex(lex_context);
}

bool test_semantic_styles() {
    Document document;
    assign(
        document,
        "#include <vector>\nclass Widget {\npublic:\n  constexpr int value = 42;\n  std::string name() const { return \"x\\n\"; }\n};\n");
    lex(CppLexer{}, document, {.begin = 0, .end = document.source.size()});

    return has_role(document, "#include", TokenRole::PREPROCESSOR) && has_role(document, "<vector>", TokenRole::MODULE) &&
           has_role(document, "class", TokenRole::KEYWORD) && has_role(document, "Widget", TokenRole::TYPE) &&
           has_role(document, "constexpr", TokenRole::KEYWORD) && has_role(document, "int", TokenRole::TYPE) &&
           has_role(document, "42", TokenRole::NUMBER) && has_role(document, "name", TokenRole::FUNCTION) &&
           has_role(document, "\\n", TokenRole::ESCAPE) && has_role(document, "return", TokenRole::KEYWORD);
}

bool test_streamed_multiline_state() {
    Document document;
    assign(document, "int prior;\n/* open");
    lex(CppLexer{}, document, {.begin = 0, .end = document.source.size()});

    const LexRange dirty = append(document, "\nstill */ Widget make();");
    if (dirty.begin != std::string_view(document.source).find("/* open")) return false;
    lex(CppLexer{}, document, dirty);

    return has_role(document, "still", TokenRole::COMMENT) && has_role(document, "Widget", TokenRole::TYPE) &&
           has_role(document, "make", TokenRole::FUNCTION);
}

bool test_raw_string_state() {
    Document document;
    assign(document, "auto text = R\"tag(first\nsecond)tag\"\nauto value = 1;\n");
    lex(CppLexer{}, document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "value", TokenRole::IDENTIFIER) && has_role(document, "1", TokenRole::NUMBER);
}

} // namespace

int main() { return test_semantic_styles() && test_streamed_multiline_state() && test_raw_string_state() ? 0 : 1; }
