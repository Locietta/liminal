#include <array>
#include <iostream>
#include <span>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/registry.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;
using namespace std::literals;

struct Resolution {
    std::string_view name;
    std::string_view id;
};

[[nodiscard]] bool resolves(std::span<const Resolution> cases) {
    for (const Resolution &item : cases) {
        const auto lexer = lexer_for_language(item.name);
        if (!lexer || (*lexer)->language_info().id != item.id) {
            std::cerr << "failed to resolve " << item.name << " as " << item.id << '\n';
            return false;
        }
    }
    return true;
}

bool resolves_every_language_identity() {
    constexpr std::array cases{
        Resolution{"cpp"sv, "cpp"sv},
        Resolution{"objc"sv, "objc"sv},
        Resolution{"resource-script"sv, "resource-script"sv},
        Resolution{"rust"sv, "rust"sv},
        Resolution{"python"sv, "python"sv},
        Resolution{"javascript"sv, "javascript"sv},
        Resolution{"actionscript"sv, "actionscript"sv},
        Resolution{"jsx"sv, "jsx"sv},
        Resolution{"typescript"sv, "typescript"sv},
        Resolution{"tsx"sv, "tsx"sv},
        Resolution{"go"sv, "go"sv},
        Resolution{"bash"sv, "bash"sv},
        Resolution{"csh"sv, "csh"sv},
        Resolution{"m4"sv, "m4"sv},
        Resolution{"json"sv, "json"sv},
        Resolution{"toml"sv, "toml"sv},
        Resolution{"yaml"sv, "yaml"sv},
        Resolution{"config"sv, "config"sv},
        Resolution{"ini"sv, "ini"sv},
        Resolution{"csv"sv, "csv"sv},
        Resolution{"tsv"sv, "tsv"sv},
        Resolution{"diff"sv, "diff"sv},
        Resolution{"html"sv, "html"sv},
        Resolution{"xml"sv, "xml"sv},
        Resolution{"markdown"sv, "markdown"sv},
        Resolution{"css"sv, "css"sv},
        Resolution{"java"sv, "java"sv},
        Resolution{"csharp"sv, "csharp"sv},
        Resolution{"dlang"sv, "d"sv},
        Resolution{"dart"sv, "dart"sv},
        Resolution{"cangjie"sv, "cangjie"sv},
        Resolution{"groovy"sv, "groovy"sv},
        Resolution{"gradle"sv, "gradle"sv},
        Resolution{"haxe"sv, "haxe"sv},
        Resolution{"kotlin"sv, "kotlin"sv},
        Resolution{"scala"sv, "scala"sv},
        Resolution{"swift"sv, "swift"sv},
        Resolution{"zig"sv, "zig"sv},
        Resolution{"asymptote"sv, "asymptote"sv},
        Resolution{"sql"sv, "sql"sv},
        Resolution{"batch"sv, "batch"sv},
        Resolution{"cmake"sv, "cmake"sv},
        Resolution{"gn"sv, "gn"sv},
        Resolution{"make"sv, "make"sv},
        Resolution{"jam"sv, "jam"sv},
        Resolution{"inno"sv, "inno"sv},
        Resolution{"nsis"sv, "nsis"sv},
        Resolution{"asm"sv, "asm"sv},
        Resolution{"cil"sv, "cil"sv},
        Resolution{"llvm"sv, "llvm"sv},
        Resolution{"smali"sv, "smali"sv},
        Resolution{"wasm"sv, "wasm"sv},
        Resolution{"verilog"sv, "verilog"sv},
        Resolution{"vhdl"sv, "vhdl"sv},
        Resolution{"winhex"sv, "winhex"sv},
        Resolution{"lisp"sv, "lisp"sv},
        Resolution{"haskell"sv, "haskell"sv},
        Resolution{"ocaml"sv, "ocaml"sv},
        Resolution{"fsharp"sv, "fsharp"sv},
        Resolution{"erlang"sv, "erlang"sv},
        Resolution{"elixir"sv, "elixir"sv},
        Resolution{"autohotkey"sv, "autohotkey"sv},
        Resolution{"autoit"sv, "autoit"sv},
        Resolution{"avisynth"sv, "avisynth"sv},
        Resolution{"awk"sv, "awk"sv},
        Resolution{"coffeescript"sv, "coffeescript"sv},
        Resolution{"julia"sv, "julia"sv},
        Resolution{"lua"sv, "lua"sv},
        Resolution{"mathematica"sv, "mathematica"sv},
        Resolution{"matlab"sv, "matlab"sv},
        Resolution{"nim"sv, "nim"sv},
        Resolution{"perl"sv, "perl"sv},
        Resolution{"php"sv, "php"sv},
        Resolution{"powershell"sv, "powershell"sv},
        Resolution{"rstats"sv, "r"sv},
        Resolution{"rebol"sv, "rebol"sv},
        Resolution{"ruby"sv, "ruby"sv},
        Resolution{"tcl"sv, "tcl"sv},
        Resolution{"vim"sv, "vim"sv},
        Resolution{"apdl"sv, "apdl"sv},
        Resolution{"abaqus"sv, "abaqus"sv},
        Resolution{"fortran"sv, "fortran"sv},
        Resolution{"pascal"sv, "pascal"sv},
        Resolution{"powerbuilder"sv, "powerbuilder"sv},
        Resolution{"sas"sv, "sas"sv},
        Resolution{"vb"sv, "vb"sv},
        Resolution{"vbscript"sv, "vbscript"sv},
        Resolution{"graphviz"sv, "graphviz"sv},
        Resolution{"blockdiag"sv, "blockdiag"sv},
        Resolution{"latex"sv, "latex"sv},
        Resolution{"rst"sv, "rst"sv},
        Resolution{"texinfo"sv, "texinfo"sv},
        Resolution{"typst"sv, "typst"sv},
    };
    return resolves(cases);
}

bool resolves_common_markdown_aliases() {
    constexpr std::array cases{
        Resolution{"C++"sv, "cpp"sv},
        Resolution{"Objective-C++"sv, "objc"sv},
        Resolution{"Python3"sv, "python"sv},
        Resolution{"as"sv, "actionscript"sv},
        Resolution{"TypeScript"sv, "typescript"sv},
        Resolution{"shellsession"sv, "bash"sv},
        Resolution{"jsonc"sv, "json"sv},
        Resolution{"yml"sv, "yaml"sv},
        Resolution{"scss"sv, "css"sv},
        Resolution{"c#"sv, "csharp"sv},
        Resolution{"kt"sv, "kotlin"sv},
        Resolution{"postgresql"sv, "sql"sv},
        Resolution{"makefile"sv, "make"sv},
        Resolution{"nasm"sv, "asm"sv},
        Resolution{"wat"sv, "wasm"sv},
        Resolution{"common-lisp"sv, "lisp"sv},
        Resolution{"f#"sv, "fsharp"sv},
        Resolution{"heex"sv, "elixir"sv},
        Resolution{"ps1"sv, "powershell"sv},
        Resolution{"delphi"sv, "pascal"sv},
        Resolution{"vb.net"sv, "vb"sv},
        Resolution{"dot"sv, "graphviz"sv},
        Resolution{"restructuredtext"sv, "rst"sv},
        Resolution{"tex"sv, "latex"sv},
    };
    return resolves(cases);
}

bool rejects_near_misses() {
    constexpr std::array names{"cppp"sv, "javascriptx"sv, "python-3"sv, "type"sv, "unknown"sv};
    for (const std::string_view name : names) {
        if (lexer_for_language(name)) {
            std::cerr << "unexpectedly resolved " << name << '\n';
            return false;
        }
    }
    return true;
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
    return resolves_every_language_identity() && resolves_common_markdown_aliases() && rejects_near_misses() &&
                   dispatches_selected_lexer() && !lexer_for_language("") ?
               0 :
               1;
}
