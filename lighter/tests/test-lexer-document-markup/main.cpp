#include <array>
#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/document_markup.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<DocumentMarkupLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, DocumentMarkupDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    DocumentMarkupLexer{.dialect = dialect}.lex(lex_context);
}

bool test_graph_languages() {
    Document graph;
    assign(graph, "digraph flow {\n  start -> finish [label=\"done\", color=green];\n}\n");
    lex(graph, DocumentMarkupDialect::GRAPHVIZ, {.begin = 0, .end = graph.source.size()});
    Document block;
    assign(block, "blockdiag {\n  orientation = portrait;\n  start -> finish;\n}\n");
    lex(block, DocumentMarkupDialect::BLOCKDIAG, {.begin = 0, .end = block.source.size()});
    return has_role(graph, "digraph", TokenRole::KEYWORD) && has_role(graph, "label", TokenRole::PROPERTY) &&
           has_role(graph, "\"done\"", TokenRole::STRING) && has_role(block, "blockdiag", TokenRole::KEYWORD) &&
           has_role(block, "orientation", TokenRole::KEYWORD);
}

bool test_document_formats() {
    Document latex;
    assign(latex, "\\documentclass{article}\n\\section{Intro}\nValue is $x + 1$. % note\n");
    lex(latex, DocumentMarkupDialect::LATEX, {.begin = 0, .end = latex.source.size()});
    Document rst;
    assign(rst, "Title\n=====\n\n.. code-block:: cpp\n\n:param value: input\n");
    lex(rst, DocumentMarkupDialect::RESTRUCTURED_TEXT, {.begin = 0, .end = rst.source.size()});
    Document texinfo;
    assign(texinfo, "@node Top\n@chapter Introduction\n@code{value}\n");
    lex(texinfo, DocumentMarkupDialect::TEXINFO, {.begin = 0, .end = texinfo.source.size()});
    Document typst;
    assign(typst, "= Heading\n#let value = 42\n#strong[Result: #value]\n");
    lex(typst, DocumentMarkupDialect::TYPST, {.begin = 0, .end = typst.source.size()});
    return has_role(latex, "\\documentclass", TokenRole::PREPROCESSOR) && has_role(latex, "\\section", TokenRole::FUNCTION) &&
           has_role(latex, "$x + 1$", TokenRole::STRING) && has_role(rst, "=====", TokenRole::MODULE) &&
           has_role(rst, ".. code-block::", TokenRole::PREPROCESSOR) && has_role(rst, ":param", TokenRole::ATTRIBUTE) &&
           has_role(texinfo, "@node", TokenRole::MODULE) && has_role(texinfo, "@chapter", TokenRole::PREPROCESSOR) &&
           has_role(typst, "=", TokenRole::MODULE) && has_role(typst, "#strong", TokenRole::PREPROCESSOR);
}

bool test_streamed_regions() {
    Document latex;
    assign(latex, "\\begin{verbatim}\nraw \\command\n");
    lex(latex, DocumentMarkupDialect::LATEX, {.begin = 0, .end = latex.source.size()});
    LexRange dirty = append(latex, "more raw\n\\end{verbatim}\n\\section{Done}\n");
    lex(latex, DocumentMarkupDialect::LATEX, dirty);
    Document typst;
    assign(typst, "/* outer\n  /* inner */\n");
    lex(typst, DocumentMarkupDialect::TYPST, {.begin = 0, .end = typst.source.size()});
    dirty = append(typst, "done */\n#let value = 1\n");
    lex(typst, DocumentMarkupDialect::TYPST, dirty);
    return has_role(latex, "raw", TokenRole::STRING) && has_role(latex, "more raw", TokenRole::STRING) &&
           has_role(latex, "\\section", TokenRole::FUNCTION, latex.source.rfind("\\section")) &&
           has_role(typst, "inner", TokenRole::COMMENT) && has_role(typst, "done", TokenRole::COMMENT) &&
           has_role(typst, "let", TokenRole::KEYWORD);
}

} // namespace

int main() { return test_graph_languages() && test_document_formats() && test_streamed_regions() ? 0 : 1; }
