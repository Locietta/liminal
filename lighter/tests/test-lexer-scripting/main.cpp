#include <array>
#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/scripting.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;
using namespace std::literals;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<ScriptingLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, ScriptingDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    ScriptingLexer{.dialect = dialect}.lex(lex_context);
}

bool test_all_keyword_profiles() {
    struct Case {
        ScriptingDialect dialect;
        std::string_view keyword;
    };
    constexpr std::array cases{
        Case{ScriptingDialect::AUTOHOTKEY, "Loop"sv},
        Case{ScriptingDialect::AUTOIT, "Func"sv},
        Case{ScriptingDialect::AVISYNTH, "function"sv},
        Case{ScriptingDialect::AWK, "BEGIN"sv},
        Case{ScriptingDialect::COFFEESCRIPT, "unless"sv},
        Case{ScriptingDialect::JULIA, "mutable"sv},
        Case{ScriptingDialect::LUA, "local"sv},
        Case{ScriptingDialect::MATHEMATICA, "Module"sv},
        Case{ScriptingDialect::MATLAB, "classdef"sv},
        Case{ScriptingDialect::NIM, "iterator"sv},
        Case{ScriptingDialect::PERL, "sub"sv},
        Case{ScriptingDialect::PHP, "namespace"sv},
        Case{ScriptingDialect::POWERSHELL, "Function"sv},
        Case{ScriptingDialect::R, "function"sv},
        Case{ScriptingDialect::REBOL, "foreach"sv},
        Case{ScriptingDialect::RUBY, "def"sv},
        Case{ScriptingDialect::TCL, "proc"sv},
        Case{ScriptingDialect::VIM, "autocmd"sv},
    };
    for (const Case &item : cases) {
        Document document;
        assign(document, item.keyword);
        lex(document, item.dialect, {.begin = 0, .end = document.source.size()});
        if (!has_role(document, item.keyword, TokenRole::KEYWORD)) return false;
        if (ScriptingLexer{.dialect = item.dialect}.language_info().id.empty()) return false;
    }
    return true;
}

bool test_declarations_and_sigils() {
    Document ruby;
    assign(ruby, "module Demo\n  class Counter\n    def add(value)\n      @total += value\n    end\n  end\nend\n");
    lex(ruby, ScriptingDialect::RUBY, {.begin = 0, .end = ruby.source.size()});
    Document powershell;
    assign(powershell, "function Get-Value($Name) { Write-Output -InputObject $Name }\n");
    lex(powershell, ScriptingDialect::POWERSHELL, {.begin = 0, .end = powershell.source.size()});
    return has_role(ruby, "Demo", TokenRole::MODULE) && has_role(ruby, "Counter", TokenRole::TYPE) &&
           has_role(ruby, "add", TokenRole::FUNCTION) && has_role(ruby, "@total", TokenRole::PROPERTY) &&
           has_role(powershell, "Get", TokenRole::FUNCTION) && has_role(powershell, "$Name", TokenRole::PROPERTY) &&
           has_role(powershell, "-InputObject", TokenRole::PARAMETER);
}

bool test_streamed_multiline_constructs() {
    Document julia;
    assign(julia, "#= outer\n   #= inner =#\n");
    lex(julia, ScriptingDialect::JULIA, {.begin = 0, .end = julia.source.size()});
    LexRange dirty = append(julia, "done =#\nfunction add(x, y)\nend\n");
    lex(julia, ScriptingDialect::JULIA, dirty);
    Document lua;
    assign(lua, "local text = [=[first\n");
    lex(lua, ScriptingDialect::LUA, {.begin = 0, .end = lua.source.size()});
    dirty = append(lua, "second]=]\nreturn text\n");
    lex(lua, ScriptingDialect::LUA, dirty);
    Document powershell;
    assign(powershell, "$text = @\"\nfirst\n");
    lex(powershell, ScriptingDialect::POWERSHELL, {.begin = 0, .end = powershell.source.size()});
    dirty = append(powershell, "second\n\"@\nreturn $text\n");
    lex(powershell, ScriptingDialect::POWERSHELL, dirty);
    return has_role(julia, "inner", TokenRole::COMMENT) && has_role(julia, "done", TokenRole::COMMENT) &&
           has_role(julia, "add", TokenRole::FUNCTION) && has_role(lua, "first", TokenRole::STRING) &&
           has_role(lua, "second", TokenRole::STRING) && has_role(lua, "return", TokenRole::KEYWORD) &&
           has_role(powershell, "first", TokenRole::STRING) && has_role(powershell, "second", TokenRole::STRING) &&
           has_role(powershell, "return", TokenRole::KEYWORD);
}

} // namespace

int main() { return test_all_keyword_profiles() && test_declarations_and_sigils() && test_streamed_multiline_constructs() ? 0 : 1; }
