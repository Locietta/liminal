#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct ScriptingDialect : u8 {
    AUTOHOTKEY,
    AUTOIT,
    AVISYNTH,
    AWK,
    COFFEESCRIPT,
    JULIA,
    LUA,
    MATHEMATICA,
    MATLAB,
    NIM,
    PERL,
    PHP,
    POWERSHELL,
    R,
    REBOL,
    RUBY,
    TCL,
    VIM,
};

struct ScriptingLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        DOCUMENTATION[[= token_role(TokenRole::DOCUMENTATION)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        VARIABLE[[= token_role(TokenRole::PROPERTY)]],
        PARAMETER[[= token_role(TokenRole::PARAMETER)]],
        OPTION[[= token_role(TokenRole::ATTRIBUTE)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        CHARACTER[[= token_role(TokenRole::CHARACTER)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    ScriptingDialect dialect = ScriptingDialect::LUA;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case ScriptingDialect::AUTOHOTKEY: return {.id = "autohotkey", .name = "AutoHotkey"};
            case ScriptingDialect::AUTOIT: return {.id = "autoit", .name = "AutoIt"};
            case ScriptingDialect::AVISYNTH: return {.id = "avisynth", .name = "AviSynth"};
            case ScriptingDialect::AWK: return {.id = "awk", .name = "Awk"};
            case ScriptingDialect::COFFEESCRIPT: return {.id = "coffeescript", .name = "CoffeeScript"};
            case ScriptingDialect::JULIA: return {.id = "julia", .name = "Julia"};
            case ScriptingDialect::LUA: return {.id = "lua", .name = "Lua"};
            case ScriptingDialect::MATHEMATICA: return {.id = "mathematica", .name = "Mathematica"};
            case ScriptingDialect::MATLAB: return {.id = "matlab", .name = "MATLAB"};
            case ScriptingDialect::NIM: return {.id = "nim", .name = "Nim"};
            case ScriptingDialect::PERL: return {.id = "perl", .name = "Perl"};
            case ScriptingDialect::PHP: return {.id = "php", .name = "PHP"};
            case ScriptingDialect::POWERSHELL: return {.id = "powershell", .name = "PowerShell"};
            case ScriptingDialect::R: return {.id = "r", .name = "R"};
            case ScriptingDialect::REBOL: return {.id = "rebol", .name = "Rebol"};
            case ScriptingDialect::RUBY: return {.id = "ruby", .name = "Ruby"};
            case ScriptingDialect::TCL: return {.id = "tcl", .name = "Tcl"};
            case ScriptingDialect::VIM: return {.id = "vim", .name = "Vim Script"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
