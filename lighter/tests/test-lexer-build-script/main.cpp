#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/build_script.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<BuildScriptLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, BuildScriptDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    BuildScriptLexer{.dialect = dialect}.lex(lex_context);
}

bool test_cmake_streaming() {
    Document document;
    assign(document, "cmake_minimum_required(VERSION 3.30)\nset(NAME ${PROJECT_NAME})\n#[=[ outer\n");
    lex(document, BuildScriptDialect::CMAKE, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "comment ]=]\nmessage(STATUS [=[ready]=])\n");
    lex(document, BuildScriptDialect::CMAKE, dirty);
    return has_role(document, "cmake_minimum_required", TokenRole::FUNCTION) &&
           has_role(document, "${PROJECT_NAME}", TokenRole::PROPERTY) && has_role(document, "outer", TokenRole::COMMENT) &&
           has_role(document, "comment", TokenRole::COMMENT) && has_role(document, "[=[ready]=]", TokenRole::STRING);
}

bool test_make_and_batch() {
    Document make;
    assign(make, "objects: $(SOURCES)\n\t$(CC) -o $@ $^\nifeq ($(DEBUG),1)\nendif\n");
    lex(make, BuildScriptDialect::MAKE, {.begin = 0, .end = make.source.size()});
    Document batch;
    assign(batch, "@echo off\n:build\nif exist %INPUT% call :compile\nrem finished\n");
    lex(batch, BuildScriptDialect::BATCH, {.begin = 0, .end = batch.source.size()});
    return has_role(make, "objects", TokenRole::LABEL) && has_role(make, "$(SOURCES)", TokenRole::PROPERTY) &&
           has_role(make, "ifeq", TokenRole::KEYWORD) && has_role(batch, ":build", TokenRole::LABEL) &&
           has_role(batch, "%INPUT%", TokenRole::PROPERTY) && has_role(batch, "finished", TokenRole::COMMENT);
}

bool test_gn_inno_and_nsis() {
    Document gn;
    assign(gn, "executable(\"app\") {\n  sources = [ \"main.cc\" ]\n}\n");
    lex(gn, BuildScriptDialect::GN, {.begin = 0, .end = gn.source.size()});
    Document inno;
    assign(inno, "[Setup]\nAppName=Liminal\n#define Version \"1.0\"\n");
    lex(inno, BuildScriptDialect::INNO_SETUP, {.begin = 0, .end = inno.source.size()});
    Document nsis;
    assign(nsis, "Section \"Main\"\n  SetOutPath $INSTDIR\nSectionEnd\n");
    lex(nsis, BuildScriptDialect::NSIS, {.begin = 0, .end = nsis.source.size()});
    return has_role(gn, "executable", TokenRole::FUNCTION) && has_role(gn, "sources", TokenRole::PROPERTY) &&
           has_role(inno, "[Setup]", TokenRole::MODULE) && has_role(inno, "AppName", TokenRole::PROPERTY) &&
           has_role(inno, "#define", TokenRole::PREPROCESSOR) && has_role(nsis, "Section", TokenRole::KEYWORD) &&
           has_role(nsis, "$INSTDIR", TokenRole::PROPERTY);
}

} // namespace

int main() { return test_cmake_streaming() && test_make_and_batch() && test_gn_inno_and_nsis() ? 0 : 1; }
