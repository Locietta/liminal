#include "registry.h"

#include <array>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/language/assembly.h>
#include <lighter/lexer/language/bash.h>
#include <lighter/lexer/language/brace.h>
#include <lighter/lexer/language/build_script.h>
#include <lighter/lexer/language/cpp.h>
#include <lighter/lexer/language/css.h>
#include <lighter/lexer/language/document_markup.h>
#include <lighter/lexer/language/functional.h>
#include <lighter/lexer/language/go.h>
#include <lighter/lexer/language/javascript.h>
#include <lighter/lexer/language/legacy.h>
#include <lighter/lexer/language/markup.h>
#include <lighter/lexer/language/python.h>
#include <lighter/lexer/language/rust.h>
#include <lighter/lexer/language/scripting.h>
#include <lighter/lexer/language/sql.h>
#include <lighter/lexer/language/structured_data.h>

namespace lighter::lexer {

namespace {

using namespace std::literals;

using Factory = Lexer (*)();

struct LanguageBinding {
    std::string_view names;
    Factory create;
};

template <auto ConfiguredLexer>
[[nodiscard]] Lexer create_configured_lexer() {
    return make_lexer(ConfiguredLexer);
}

// Each binding lists canonical Markdown fence names and common ecosystem
// aliases separated by '|'. The table is deliberately explicit: adding a
// language is an ordinary constexpr edit with no generated catalogue or
// registration macro.
constexpr std::array k_registry = {
    LanguageBinding{"c|c++|cc|cpp|cxx|h|hh|hpp|hxx|idl|midl|odl"sv, create_configured_lexer<CppLexer{.dialect = CppDialect::CPP}>},
    LanguageBinding{"m|mm|obj-c|obj-c++|objc|objc++|objcpp|objective-c|objective-c++|objective-cpp"sv,
                    create_configured_lexer<CppLexer{.dialect = CppDialect::OBJECTIVE_C}>},
    LanguageBinding{"dlg|rc|rc2|rct|resource|resource-script|rh"sv,
                    create_configured_lexer<CppLexer{.dialect = CppDialect::RESOURCE_SCRIPT}>},
    LanguageBinding{"rs|rust"sv, create_configured_lexer<RustLexer{}>},
    LanguageBinding{"py|py3|pycon|python|python3"sv, create_configured_lexer<PythonLexer{}>},
    LanguageBinding{"cjs|javascript|js|jse|jsm|mjs|qs"sv,
                    create_configured_lexer<JavaScriptLexer{.dialect = JavaScriptDialect::JAVASCRIPT}>},
    LanguageBinding{"actionscript|as|as3"sv, create_configured_lexer<JavaScriptLexer{.dialect = JavaScriptDialect::ACTIONSCRIPT}>},
    LanguageBinding{"jsx"sv, create_configured_lexer<JavaScriptLexer{.dialect = JavaScriptDialect::JSX}>},
    LanguageBinding{"cts|ets|mts|osts|ts|typescript"sv, create_configured_lexer<JavaScriptLexer{.dialect = JavaScriptDialect::TYPESCRIPT}>},
    LanguageBinding{"tsx"sv, create_configured_lexer<JavaScriptLexer{.dialect = JavaScriptDialect::TSX}>},
    LanguageBinding{"go|golang|gop|ql"sv, create_configured_lexer<GoLexer{}>},

    LanguageBinding{"bash|dash|ksh|sh|shell|shell-script|shellsession|zsh"sv,
                    create_configured_lexer<BashLexer{.dialect = ShellDialect::BASH}>},
    LanguageBinding{"csh|tcsh"sv, create_configured_lexer<BashLexer{.dialect = ShellDialect::C_SHELL}>},
    LanguageBinding{"autoconf|m4"sv, create_configured_lexer<BashLexer{.dialect = ShellDialect::M4}>},

    LanguageBinding{"geojson|ipynb|json|json5|jsonc|jsonl|ndjson|webmanifest"sv,
                    create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::JSON}>},
    LanguageBinding{"toml"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::TOML}>},
    LanguageBinding{"yaml|yml"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::YAML}>},
    LanguageBinding{"cfg|conf|config|dotenv|editorconfig|env|properties"sv,
                    create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::CONFIG}>},
    LanguageBinding{"desktop|ini"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::INI}>},
    LanguageBinding{"csv"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::CSV}>},
    LanguageBinding{"tsv"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::TSV}>},
    LanguageBinding{"diff|patch|udiff"sv, create_configured_lexer<StructuredDataLexer{.dialect = StructuredDataDialect::DIFF}>},

    LanguageBinding{"htm|html|shtml|vue|xhtml"sv, create_configured_lexer<MarkupLexer{.dialect = MarkupDialect::HTML}>},
    LanguageBinding{"csproj|plist|rss|svg|xaml|xml|xsd|xsl|xslt"sv, create_configured_lexer<MarkupLexer{.dialect = MarkupDialect::XML}>},
    LanguageBinding{"markdown|md|mdown|mkd|mkdn"sv, create_configured_lexer<MarkupLexer{.dialect = MarkupDialect::MARKDOWN}>},
    LanguageBinding{"css|less|sass|scss"sv, create_configured_lexer<CssLexer{}>},

    LanguageBinding{"java"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::JAVA}>},
    LanguageBinding{"c#|cs|csharp|dotnet"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::CSHARP}>},
    LanguageBinding{"d|dlang"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::D}>},
    LanguageBinding{"dart"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::DART}>},
    LanguageBinding{"cangjie|cj"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::CANGJIE}>},
    LanguageBinding{"groovy"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::GROOVY}>},
    LanguageBinding{"gradle"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::GRADLE}>},
    LanguageBinding{"haxe|hx"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::HAXE}>},
    LanguageBinding{"kotlin|kt|kts"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::KOTLIN}>},
    LanguageBinding{"scala|sc"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::SCALA}>},
    LanguageBinding{"swift"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::SWIFT}>},
    LanguageBinding{"zig"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::ZIG}>},
    LanguageBinding{"asymptote|asy"sv, create_configured_lexer<BraceLexer{.dialect = BraceDialect::ASYMPTOTE}>},

    LanguageBinding{"mysql|pgsql|plsql|postgres|postgresql|sqlite|sql|tsql"sv, create_configured_lexer<SqlLexer{}>},
    LanguageBinding{"bat|batch|cmd|dosbatch"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::BATCH}>},
    LanguageBinding{"cmake|cmake.in"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::CMAKE}>},
    LanguageBinding{"gn|gni"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::GN}>},
    LanguageBinding{"make|makefile|mk"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::MAKE}>},
    LanguageBinding{"bjam|boost-jam|jam|jamfile"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::JAM}>},
    LanguageBinding{"inno|iss"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::INNO_SETUP}>},
    LanguageBinding{"nsi|nsis|nsh"sv, create_configured_lexer<BuildScriptLexer{.dialect = BuildScriptDialect::NSIS}>},

    LanguageBinding{"aarch64asm|asm|assembly|gas|masm|nasm|x86asm"sv,
                    create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::ASSEMBLY}>},
    LanguageBinding{"cil|il|msil"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::CIL}>},
    LanguageBinding{"llvm|llvm-ir"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::LLVM_IR}>},
    LanguageBinding{"smali"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::SMALI}>},
    LanguageBinding{"wasm|wat|webassembly"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::WEBASSEMBLY}>},
    LanguageBinding{"sv|svh|systemverilog|v|verilog"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::VERILOG}>},
    LanguageBinding{"vhd|vhdl"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::VHDL}>},
    LanguageBinding{"whs|winhex"sv, create_configured_lexer<AssemblyLexer{.dialect = AssemblyDialect::WINHEX}>},

    LanguageBinding{"cl|clojure|common-lisp|elisp|emacs-lisp|lisp|racket|scheme"sv,
                    create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::LISP}>},
    LanguageBinding{"haskell|hs|lhs"sv, create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::HASKELL}>},
    LanguageBinding{"ml|mli|ocaml"sv, create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::OCAML}>},
    LanguageBinding{"f#|fs|fsharp|fsi|fsx"sv, create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::FSHARP}>},
    LanguageBinding{"erl|erlang|hrl"sv, create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::ERLANG}>},
    LanguageBinding{"eex|elixir|ex|exs|heex"sv, create_configured_lexer<FunctionalLexer{.dialect = FunctionalDialect::ELIXIR}>},

    LanguageBinding{"ahk|autohotkey"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::AUTOHOTKEY}>},
    LanguageBinding{"au3|autoit"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::AUTOIT}>},
    LanguageBinding{"avisynth|avs"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::AVISYNTH}>},
    LanguageBinding{"awk|gawk"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::AWK}>},
    LanguageBinding{"coffee|coffeescript"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::COFFEESCRIPT}>},
    LanguageBinding{"jl|julia"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::JULIA}>},
    LanguageBinding{"lua"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::LUA}>},
    LanguageBinding{"mathematica|mma|nb|wolfram|wolfram-language"sv,
                    create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::MATHEMATICA}>},
    LanguageBinding{"matlab"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::MATLAB}>},
    LanguageBinding{"nim|nimrod"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::NIM}>},
    LanguageBinding{"perl|pl|pm"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::PERL}>},
    LanguageBinding{"hack|php|php3|php4|php5|phtml"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::PHP}>},
    LanguageBinding{"powershell|ps1|psd1|psm1|pwsh"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::POWERSHELL}>},
    LanguageBinding{"r|rstats"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::R}>},
    LanguageBinding{"rebol|r3"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::REBOL}>},
    LanguageBinding{"erb|gemspec|rake|rb|ruby"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::RUBY}>},
    LanguageBinding{"tcl|tk"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::TCL}>},
    LanguageBinding{"vim|viml|vimscript"sv, create_configured_lexer<ScriptingLexer{.dialect = ScriptingDialect::VIM}>},

    LanguageBinding{"ansys|apdl"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::APDL}>},
    LanguageBinding{"abaqus|inp"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::ABAQUS}>},
    LanguageBinding{"f|f03|f08|f77|f90|f95|for|fortran"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::FORTRAN}>},
    LanguageBinding{"delphi|freepascal|objectpascal|pas|pascal"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::PASCAL}>},
    LanguageBinding{"pbl|powerbuilder|powerscript"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::POWERBUILDER}>},
    LanguageBinding{"sas"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::SAS}>},
    LanguageBinding{"vb|vb.net|vbnet|visual-basic|visualbasic"sv,
                    create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::VISUAL_BASIC}>},
    LanguageBinding{"vbs|vbscript"sv, create_configured_lexer<LegacyLexer{.dialect = LegacyDialect::VBSCRIPT}>},

    LanguageBinding{"dot|dotlang|graphviz|gv"sv, create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::GRAPHVIZ}>},
    LanguageBinding{"actdiag|blockdiag|nwdiag|packetdiag|rackdiag|seqdiag"sv,
                    create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::BLOCKDIAG}>},
    LanguageBinding{"latex|tex"sv, create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::LATEX}>},
    LanguageBinding{"rest|restructuredtext|rst"sv,
                    create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::RESTRUCTURED_TEXT}>},
    LanguageBinding{"texi|texinfo"sv, create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::TEXINFO}>},
    LanguageBinding{"typ|typst"sv, create_configured_lexer<DocumentMarkupLexer{.dialect = DocumentMarkupDialect::TYPST}>},
};

[[nodiscard]] bool equal_ignoring_ascii_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index])) return false;
    }
    return true;
}

[[nodiscard]] bool contains_name(std::string_view names, std::string_view requested) noexcept {
    while (!names.empty()) {
        const usize separator = names.find('|');
        const std::string_view candidate = names.substr(0, separator);
        if (equal_ignoring_ascii_case(candidate, requested)) return true;
        if (separator == std::string_view::npos) break;
        names.remove_prefix(separator + 1);
    }
    return false;
}

} // namespace

std::optional<Lexer> lexer_for_language(std::string_view name) {
    for (const LanguageBinding &binding : k_registry) {
        if (contains_name(binding.names, name)) return binding.create();
    }
    return std::nullopt;
}

} // namespace lighter::lexer
