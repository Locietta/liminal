#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/functional.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<FunctionalLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got "
                      << static_cast<int>(role_for_style<FunctionalLexer::Style>(document.styles[offset])) << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, FunctionalDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    FunctionalLexer{.dialect = dialect}.lex(lex_context);
}

bool test_lisp_and_haskell() {
    Document lisp;
    assign(lisp, "(defun greet (name) (format t \"Hello ~A\" name))\n#| outer #| inner |#\n");
    lex(lisp, FunctionalDialect::LISP, {.begin = 0, .end = lisp.source.size()});
    const LexRange dirty = append(lisp, "done |#\n");
    lex(lisp, FunctionalDialect::LISP, dirty);
    Document haskell;
    assign(haskell, "module Main where\nmapMaybe :: (a -> Maybe b) -> [a] -> [b]\nmapMaybe f = foldr step []\n");
    lex(haskell, FunctionalDialect::HASKELL, {.begin = 0, .end = haskell.source.size()});
    return has_role(lisp, "defun", TokenRole::KEYWORD) && has_role(lisp, "greet", TokenRole::FUNCTION) &&
           has_role(lisp, "inner", TokenRole::COMMENT) && has_role(lisp, "done", TokenRole::COMMENT) &&
           has_role(haskell, "module", TokenRole::KEYWORD) && has_role(haskell, "Main", TokenRole::MODULE) &&
           has_role(haskell, "mapMaybe", TokenRole::FUNCTION) && has_role(haskell, "Maybe", TokenRole::TYPE, haskell.source.find("::"));
}

bool test_ml_languages() {
    Document ocaml;
    assign(ocaml, "module Store = struct\n  type item = { name : string }\n  let find ~name = None\nend\n");
    lex(ocaml, FunctionalDialect::OCAML, {.begin = 0, .end = ocaml.source.size()});
    Document fsharp;
    assign(fsharp, "namespace Demo\nmodule Store =\n    let find name = Some name\n");
    lex(fsharp, FunctionalDialect::FSHARP, {.begin = 0, .end = fsharp.source.size()});
    return has_role(ocaml, "Store", TokenRole::MODULE) && has_role(ocaml, "item", TokenRole::TYPE) &&
           has_role(ocaml, "~name", TokenRole::LABEL) && has_role(fsharp, "Demo", TokenRole::MODULE) &&
           has_role(fsharp, "Store", TokenRole::MODULE) && has_role(fsharp, "find", TokenRole::FUNCTION);
}

bool test_beam_languages() {
    Document erlang;
    assign(erlang, "-module(counter).\n-export([add/2]).\nadd(Left, Right) -> Left + Right.\n");
    lex(erlang, FunctionalDialect::ERLANG, {.begin = 0, .end = erlang.source.size()});
    Document elixir;
    assign(elixir,
           "defmodule Demo.Counter do\n  @spec add(integer(), integer()) :: integer()\n  def add(left, right), do: left + right\nend\n");
    lex(elixir, FunctionalDialect::ELIXIR, {.begin = 0, .end = elixir.source.size()});
    return has_role(erlang, "-module", TokenRole::PREPROCESSOR) &&
           has_role(erlang, "add", TokenRole::FUNCTION, erlang.source.find("add(Left")) && has_role(erlang, "Left", TokenRole::PARAMETER) &&
           has_role(elixir, "defmodule", TokenRole::KEYWORD) && has_role(elixir, "Demo", TokenRole::MODULE) &&
           has_role(elixir, "@spec", TokenRole::ATTRIBUTE) && has_role(elixir, "add", TokenRole::FUNCTION, elixir.source.find("def add"));
}

} // namespace

int main() { return test_lisp_and_haskell() && test_ml_languages() && test_beam_languages() ? 0 : 1; }
