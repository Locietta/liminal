#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/structured_data.h>
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
        const TokenRole actual = role_for_style<StructuredDataLexer::Style>(document.styles[offset]);
        if (actual != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got " << static_cast<int>(actual)
                      << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, StructuredDataDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    StructuredDataLexer{.dialect = dialect}.lex(lex_context);
}

bool test_json() {
    Document document;
    assign(document, "{\n  // JSON5\n  \"name\": \"liminal\\n\",\n  enabled: true,\n  count: 42\n}\n");
    lex(document, StructuredDataDialect::JSON, {.begin = 0, .end = document.source.size()});
    return has_role(document, "// JSON5", TokenRole::COMMENT) && has_role(document, "\"name\"", TokenRole::PROPERTY) &&
           has_role(document, "\\n", TokenRole::ESCAPE) && has_role(document, "enabled", TokenRole::PROPERTY) &&
           has_role(document, "true", TokenRole::KEYWORD) && has_role(document, "42", TokenRole::NUMBER);
}

bool test_toml_streaming() {
    Document document;
    assign(document, "[package]\nname = \"liminal\"\ndescription = \"\"\"first\n");
    lex(document, StructuredDataDialect::TOML, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "second\"\"\"\nenabled = true\n");
    lex(document, StructuredDataDialect::TOML, dirty);
    return has_role(document, "[package]", TokenRole::MODULE) && has_role(document, "name", TokenRole::PROPERTY) &&
           has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "true", TokenRole::KEYWORD);
}

bool test_yaml_block() {
    Document document;
    assign(document, "---\nservice: &main liminal\nenabled: true\nmessage: |\n  first line\n");
    lex(document, StructuredDataDialect::YAML, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "  second line\nnext: 7\n");
    lex(document, StructuredDataDialect::YAML, dirty);
    return has_role(document, "---", TokenRole::MODULE) && has_role(document, "service", TokenRole::PROPERTY) &&
           has_role(document, "&main", TokenRole::LABEL) && has_role(document, "true", TokenRole::KEYWORD) &&
           has_role(document, "second line", TokenRole::STRING) && has_role(document, "next", TokenRole::PROPERTY) &&
           has_role(document, "7", TokenRole::NUMBER);
}

bool test_properties_csv_and_diff() {
    Document ini;
    assign(ini, "[agent]\nname = \"liminal\"\nenabled = true\n");
    lex(ini, StructuredDataDialect::INI, {.begin = 0, .end = ini.source.size()});

    Document csv;
    assign(csv, "name,value\n\"multi\nline\",42\n");
    lex(csv, StructuredDataDialect::CSV, {.begin = 0, .end = csv.source.size()});

    Document diff;
    assign(diff, "diff --git a/a b/a\n@@ -1 +1 @@\n-old\n+new\n");
    lex(diff, StructuredDataDialect::DIFF, {.begin = 0, .end = diff.source.size()});
    return has_role(ini, "[agent]", TokenRole::MODULE) && has_role(ini, "name", TokenRole::PROPERTY) &&
           has_role(ini, "true", TokenRole::KEYWORD) && has_role(csv, "multi", TokenRole::STRING) &&
           has_role(csv, "42", TokenRole::NUMBER) && has_role(diff, "diff --git", TokenRole::MODULE) &&
           has_role(diff, "@@ -1 +1 @@", TokenRole::LABEL) && has_role(diff, "-old", TokenRole::COMMENT) &&
           has_role(diff, "+new", TokenRole::STRING);
}

} // namespace

int main() { return test_json() && test_toml_streaming() && test_yaml_block() && test_properties_csv_and_diff() ? 0 : 1; }
