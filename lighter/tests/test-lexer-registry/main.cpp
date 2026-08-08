#include <lighter/lexer/document.h>
#include <lighter/lexer/registry.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

bool resolves_aliases() {
    auto c = lexer_for_language("c");
    auto cpp = lexer_for_language("C++");
    auto objective_c = lexer_for_language("objc");
    auto objective_cpp = lexer_for_language("Objective-C++");
    auto resource = lexer_for_language("rc");
    auto idl = lexer_for_language("odl");
    auto rust = lexer_for_language("rs");
    auto python = lexer_for_language("Python3");
    return c && (*c)->language_info().id == "c" && cpp && (*cpp)->language_info().id == "cpp" && objective_c &&
           (*objective_c)->language_info().id == "objective-c" && objective_cpp &&
           (*objective_cpp)->language_info().id == "objective-cpp" && resource && (*resource)->language_info().id == "resource-script" &&
           idl && (*idl)->language_info().id == "idl" && rust && (*rust)->language_info().id == "rust" && python &&
           (*python)->language_info().id == "python";
}

bool dispatches_selected_lexer() {
    auto lexer = lexer_for_language("cpp");
    if (!lexer) return false;

    Document document;
    assign(document, "struct Widget {};");
    auto lex_context = context(document, {.begin = 0, .end = document.source.size()});
    (*lexer)->lex(lex_context);
    return (*lexer)->role_for_style(document.styles[0]) == TokenRole::KEYWORD;
}

} // namespace

int main() {
    return resolves_aliases() && dispatches_selected_lexer() && !lexer_for_language("") && !lexer_for_language("unknown") ? 0 : 1;
}
