#include <array>
#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/legacy.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;
using namespace std::literals;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<LegacyLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, LegacyDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    LegacyLexer{.dialect = dialect}.lex(lex_context);
}

bool test_all_profiles() {
    struct Case {
        LegacyDialect dialect;
        std::string_view keyword;
    };
    constexpr std::array cases{
        Case{LegacyDialect::APDL, "ANTYPE"sv},           Case{LegacyDialect::ABAQUS, "SOLVE"sv},
        Case{LegacyDialect::FORTRAN, "SUBROUTINE"sv},    Case{LegacyDialect::PASCAL, "begin"sv},
        Case{LegacyDialect::POWERBUILDER, "forward"sv},  Case{LegacyDialect::SAS, "PROC"sv},
        Case{LegacyDialect::VISUAL_BASIC, "Function"sv}, Case{LegacyDialect::VBSCRIPT, "Dim"sv},
    };
    for (const Case &item : cases) {
        Document document;
        assign(document, item.keyword);
        lex(document, item.dialect, {.begin = 0, .end = document.source.size()});
        if (!has_role(document, item.keyword, TokenRole::KEYWORD)) return false;
        if (LegacyLexer{.dialect = item.dialect}.language_info().id.empty()) return false;
    }
    return true;
}

bool test_declarations() {
    Document fortran;
    assign(fortran, "module vectors\ncontains\n  function length(value) result(out)\n    real :: out\n  end function\nend module\n");
    lex(fortran, LegacyDialect::FORTRAN, {.begin = 0, .end = fortran.source.size()});
    Document vb;
    assign(vb, "Namespace Demo\nClass Counter\nPublic Function Add(value As Integer) As Integer\nEnd Function\nEnd Class\nEnd Namespace\n");
    lex(vb, LegacyDialect::VISUAL_BASIC, {.begin = 0, .end = vb.source.size()});
    return has_role(fortran, "vectors", TokenRole::MODULE) && has_role(fortran, "length", TokenRole::FUNCTION) &&
           has_role(fortran, "real", TokenRole::TYPE) && has_role(vb, "Demo", TokenRole::MODULE) &&
           has_role(vb, "Counter", TokenRole::TYPE) && has_role(vb, "Add", TokenRole::FUNCTION) && has_role(vb, "Integer", TokenRole::TYPE);
}

bool test_streamed_comments_and_directives() {
    Document pascal;
    assign(pascal, "program Demo;\n{ outer\n");
    lex(pascal, LegacyDialect::PASCAL, {.begin = 0, .end = pascal.source.size()});
    LexRange dirty = append(pascal, "comment }\nbegin\nend.\n");
    lex(pascal, LegacyDialect::PASCAL, dirty);
    Document sas;
    assign(sas, "%macro report(name);\n/* outer\n");
    lex(sas, LegacyDialect::SAS, {.begin = 0, .end = sas.source.size()});
    dirty = append(sas, "comment */\n%put &name;\n%mend;\n");
    lex(sas, LegacyDialect::SAS, dirty);
    Document abaqus;
    assign(abaqus, "*HEADING\n** generated model\n*NODE\n1, 0., 0., 0.\n");
    lex(abaqus, LegacyDialect::ABAQUS, {.begin = 0, .end = abaqus.source.size()});
    return has_role(pascal, "outer", TokenRole::COMMENT) && has_role(pascal, "comment", TokenRole::COMMENT) &&
           has_role(pascal, "begin", TokenRole::KEYWORD) && has_role(sas, "%macro", TokenRole::PREPROCESSOR) &&
           has_role(sas, "comment", TokenRole::COMMENT) && has_role(sas, "&name", TokenRole::PROPERTY) &&
           has_role(abaqus, "*HEADING", TokenRole::PREPROCESSOR) && has_role(abaqus, "generated model", TokenRole::COMMENT);
}

} // namespace

int main() { return test_all_profiles() && test_declarations() && test_streamed_comments_and_directives() ? 0 : 1; }
