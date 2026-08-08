#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/sql.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<SqlLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, LexRange range) {
    auto lex_context = context(document, range);
    SqlLexer{}.lex(lex_context);
}

bool test_semantic_sql() {
    Document document;
    assign(document, "CREATE TABLE app.users (id bigint PRIMARY KEY, name varchar(100));\n"
                     "SELECT users.name, count(*) FROM app.users WHERE id = :id AND active = true;\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    return has_role(document, "CREATE", TokenRole::KEYWORD) && has_role(document, "app", TokenRole::MODULE) &&
           has_role(document, "bigint", TokenRole::TYPE) && has_role(document, "varchar", TokenRole::TYPE) &&
           has_role(document, "SELECT", TokenRole::KEYWORD) &&
           has_role(document, "name", TokenRole::PROPERTY, document.source.find("SELECT")) &&
           has_role(document, "count", TokenRole::FUNCTION) && has_role(document, ":id", TokenRole::PARAMETER) &&
           has_role(document, "true", TokenRole::CONSTANT);
}

bool test_streamed_comment_and_dollar_string() {
    Document document;
    assign(document, "/* outer\n  /* inner */\n");
    lex(document, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "*/\nSELECT $$first\nsecond$$;\n");
    lex(document, dirty);
    return has_role(document, "inner", TokenRole::COMMENT) && has_role(document, "first", TokenRole::STRING) &&
           has_role(document, "second", TokenRole::STRING) &&
           has_role(document, "SELECT", TokenRole::KEYWORD, document.source.find("*/\n"));
}

} // namespace

int main() { return test_semantic_sql() && test_streamed_comment_and_dollar_string() ? 0 : 1; }
