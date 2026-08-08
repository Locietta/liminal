#pragma once

#include <lighter/lexer/lexer.h>

namespace lighter::lexer {

enum struct AssemblyDialect : u8 {
    ASSEMBLY,
    CIL,
    LLVM_IR,
    SMALI,
    WEBASSEMBLY,
    VERILOG,
    VHDL,
    WINHEX,
};

struct AssemblyLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        COMMENT[[= token_role(TokenRole::COMMENT)]],
        KEYWORD[[= token_role(TokenRole::KEYWORD)]],
        TYPE[[= token_role(TokenRole::TYPE)]],
        INSTRUCTION[[= token_role(TokenRole::FUNCTION)]],
        FUNCTION[[= token_role(TokenRole::FUNCTION)]],
        IDENTIFIER[[= token_role(TokenRole::IDENTIFIER)]],
        REGISTER[[= token_role(TokenRole::PARAMETER)]],
        PROPERTY[[= token_role(TokenRole::PROPERTY)]],
        CONSTANT[[= token_role(TokenRole::CONSTANT)]],
        STRING[[= token_role(TokenRole::STRING)]],
        ESCAPE[[= token_role(TokenRole::ESCAPE)]],
        NUMBER[[= token_role(TokenRole::NUMBER)]],
        OPERATOR[[= token_role(TokenRole::OPERATOR)]],
        DIRECTIVE[[= token_role(TokenRole::PREPROCESSOR)]],
        LABEL[[= token_role(TokenRole::LABEL)]],
        MODULE[[= token_role(TokenRole::MODULE)]],
        ATTRIBUTE[[= token_role(TokenRole::ATTRIBUTE)]],
        ERROR[[= token_role(TokenRole::UNRECOGNIZED)]],
    };

    AssemblyDialect dialect = AssemblyDialect::ASSEMBLY;

    [[nodiscard]] constexpr LanguageInfo language_info() const noexcept {
        switch (dialect) {
            case AssemblyDialect::ASSEMBLY: return {.id = "asm", .name = "Assembly"};
            case AssemblyDialect::CIL: return {.id = "cil", .name = "Common Intermediate Language"};
            case AssemblyDialect::LLVM_IR: return {.id = "llvm", .name = "LLVM IR"};
            case AssemblyDialect::SMALI: return {.id = "smali", .name = "Smali"};
            case AssemblyDialect::WEBASSEMBLY: return {.id = "wasm", .name = "WebAssembly Text"};
            case AssemblyDialect::VERILOG: return {.id = "verilog", .name = "Verilog"};
            case AssemblyDialect::VHDL: return {.id = "vhdl", .name = "VHDL"};
            case AssemblyDialect::WINHEX: return {.id = "winhex", .name = "WinHex Script"};
        }
        return {};
    }

    void lex(LexContext &context) const;
};

} // namespace lighter::lexer
