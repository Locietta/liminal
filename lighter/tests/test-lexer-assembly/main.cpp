#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/assembly.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) return false;
    for (usize offset = position; offset < position + token.size(); ++offset) {
        if (role_for_style<AssemblyLexer::Style>(document.styles[offset]) != role) {
            std::cerr << "role mismatch for " << token << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, AssemblyDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    AssemblyLexer{.dialect = dialect}.lex(lex_context);
}

bool test_assembly_and_ir() {
    Document assembly;
    assign(assembly, "start:\n  mov rax, [rbx + 8] ; load value\n  ret\n");
    lex(assembly, AssemblyDialect::ASSEMBLY, {.begin = 0, .end = assembly.source.size()});
    Document ir;
    assign(ir, "define i32 @sum(i32 %left, i32 %right) {\n  %value = add i32 %left, %right\n  ret i32 %value\n}\n");
    lex(ir, AssemblyDialect::LLVM_IR, {.begin = 0, .end = ir.source.size()});
    return has_role(assembly, "start", TokenRole::LABEL) && has_role(assembly, "mov", TokenRole::FUNCTION) &&
           has_role(assembly, "rax", TokenRole::PARAMETER) && has_role(assembly, "load value", TokenRole::COMMENT) &&
           has_role(ir, "define", TokenRole::KEYWORD) && has_role(ir, "i32", TokenRole::TYPE) &&
           has_role(ir, "@sum", TokenRole::FUNCTION) && has_role(ir, "%value", TokenRole::PARAMETER) &&
           has_role(ir, "add", TokenRole::FUNCTION);
}

bool test_streamed_wasm_comment() {
    Document document;
    assign(document, "(module\n  (; outer (; inner ;)\n");
    lex(document, AssemblyDialect::WEBASSEMBLY, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "  done ;)\n  (func $run (result i32) i32.const 1)\n)\n");
    lex(document, AssemblyDialect::WEBASSEMBLY, dirty);
    return has_role(document, "module", TokenRole::KEYWORD) && has_role(document, "inner", TokenRole::COMMENT) &&
           has_role(document, "done", TokenRole::COMMENT) && has_role(document, "$run", TokenRole::PARAMETER) &&
           has_role(document, "i32.const", TokenRole::FUNCTION);
}

bool test_smali_and_hdl() {
    Document smali;
    assign(smali, ".method public static main()V\n  const-string v0, \"hello\"\n  return v0\n.end method\n");
    lex(smali, AssemblyDialect::SMALI, {.begin = 0, .end = smali.source.size()});
    Document verilog;
    assign(verilog, "module counter(input logic clk);\n  always_ff @(posedge clk) value <= value + 1'b1;\nendmodule\n");
    lex(verilog, AssemblyDialect::VERILOG, {.begin = 0, .end = verilog.source.size()});
    Document vhdl;
    assign(vhdl, "entity Counter is\n  port (clk : in std_logic);\nend entity;\n");
    lex(vhdl, AssemblyDialect::VHDL, {.begin = 0, .end = vhdl.source.size()});
    return has_role(smali, ".method", TokenRole::PREPROCESSOR) && has_role(smali, "const-string", TokenRole::FUNCTION) &&
           has_role(smali, "v0", TokenRole::PARAMETER) && has_role(verilog, "module", TokenRole::KEYWORD) &&
           has_role(verilog, "counter", TokenRole::MODULE) && has_role(verilog, "logic", TokenRole::TYPE) &&
           has_role(vhdl, "entity", TokenRole::KEYWORD) && has_role(vhdl, "Counter", TokenRole::MODULE) &&
           has_role(vhdl, "std_logic", TokenRole::TYPE);
}

} // namespace

int main() { return test_assembly_and_ir() && test_streamed_wasm_comment() && test_smali_and_hdl() ? 0 : 1; }
