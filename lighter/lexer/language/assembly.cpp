#include "assembly.h"

#include <algorithm>
#include <string_view>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/scanner.h>
#include <lighter/lexer/word_set.h>

// Derived from Notepad4's LexAsm.cxx, LexCIL.cxx, LexLLVM.cxx, LexSmali.cxx,
// LexWASM.cxx, LexVerilog.cxx, LexVHDL.cxx, LexWinHex.cxx, and matching
// stl*.cpp data at revision eee400c824b30e0aa41ef06a18ce22cf69b5cbb0.
// This port shares byte traversal while preserving dialect-specific comments,
// directives, opcodes, registers, symbol prefixes, types, and nested WAT
// comments. Folding and editor-only indicators are omitted.
// See lighter/lexer/THIRD_PARTY_NOTICES.md.
namespace lighter::lexer {

namespace {

using Dialect = AssemblyDialect;
using Style = AssemblyLexer::Style;

constexpr auto k_asm_instructions =
    make_word_set("adc", "add", "and", "call", "cmp", "dec", "div", "enter", "idiv", "imul", "inc", "int", "ja", "jae", "jb", "jbe", "je",
                  "jg", "jge", "jl", "jle", "jmp", "jne", "lea", "leave", "mov", "movsx", "movzx", "mul", "neg", "nop", "not", "or", "pop",
                  "push", "ret", "rol", "ror", "sal", "sar", "sbb", "shl", "shr", "sub", "syscall", "test", "xor");
constexpr auto k_asm_registers =
    make_word_set("ah", "al", "ax", "bh", "bl", "bp", "bpl", "bx", "ch", "cl", "cr0", "cr2", "cr3", "cr4", "cs", "cx", "dh", "di", "dil",
                  "dl", "dr0", "dr1", "dr2", "dr3", "dr6", "dr7", "ds", "dx", "eax", "ebp", "ebx", "ecx", "edi", "edx", "eip", "es", "esi",
                  "esp", "fs", "gs", "ip", "r10", "r11", "r12", "r13", "r14", "r15", "r8", "r9", "rax", "rbp", "rbx", "rcx", "rdi", "rdx",
                  "rip", "rsi", "rsp", "si", "sil", "sp", "spl", "ss", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
constexpr auto k_cil_keywords =
    make_word_set("abstract", "algorithm", "assembly", "auto", "beforefieldinit", "cil", "class", "default", "enum", "explicit", "extends",
                  "field", "final", "implements", "instance", "interface", "managed", "method", "nested", "private", "property", "public",
                  "runtime", "sealed", "sequential", "specialname", "static", "unicode", "unmanaged", "value");
constexpr auto k_cil_types = make_word_set("bool", "char", "class", "float32", "float64", "int16", "int32", "int64", "int8", "native",
                                           "object", "string", "typedref", "uint16", "uint32", "uint64", "uint8", "valuetype", "void");
constexpr auto k_ir_keywords =
    make_word_set("alias", "attributes", "comdat", "constant", "declare", "define", "distinct", "external", "global", "ifunc", "internal",
                  "module", "private", "source_filename", "target", "thread_local", "type", "unnamed_addr");
constexpr auto k_ir_types = make_word_set("bfloat", "double", "float", "fp128", "half", "i1", "i128", "i16", "i32", "i64", "i8", "label",
                                          "metadata", "opaque", "ptr", "token", "void", "x86_fp80");
constexpr auto k_ir_instructions = make_word_set(
    "add", "addrspacecast", "alloca", "and", "ashr", "atomicrmw", "bitcast", "br", "call", "callbr", "catchpad", "catchret", "catchswitch",
    "cleanuppad", "cleanupret", "cmpxchg", "extractelement", "extractvalue", "fadd", "fcmp", "fdiv", "fence", "fmul", "fneg", "fpext",
    "fptosi", "fptoui", "fptrunc", "frem", "fsub", "getelementptr", "icmp", "indirectbr", "insertelement", "insertvalue", "inttoptr",
    "invoke", "landingpad", "load", "lshr", "mul", "or", "phi", "ptrtoint", "resume", "ret", "sdiv", "select", "sext", "shl",
    "shufflevector", "sitofp", "srem", "store", "sub", "switch", "trunc", "udiv", "uitofp", "unreachable", "urem", "va_arg", "xor", "zext");
constexpr auto k_smali_instructions =
    make_word_set("add-int", "aget", "and-int", "aput", "array-length", "check-cast", "cmp-long", "const", "const-string", "div-int",
                  "fill-array-data", "goto", "if-eq", "if-nez", "iget", "instance-of", "invoke-direct", "invoke-interface", "invoke-static",
                  "invoke-super", "invoke-virtual", "iput", "monitor-enter", "monitor-exit", "move", "move-exception", "move-result",
                  "mul-int", "neg-int", "new-array", "new-instance", "nop", "not-int", "or-int", "packed-switch", "rem-int", "return",
                  "return-object", "sget", "sparse-switch", "sput", "sub-int", "throw", "ushr-int", "xor-int");
constexpr auto k_wasm_keywords = make_word_set("array", "data", "elem", "export", "func", "global", "import", "memory", "module", "mut",
                                               "param", "rec", "result", "start", "struct", "table", "tag", "type");
constexpr auto k_wasm_instructions =
    make_word_set("block", "br", "br_if", "br_table", "call", "call_indirect", "data.drop", "drop", "elem.drop", "else", "end",
                  "global.get", "global.set", "i32.add", "i32.const", "i32.load", "i32.store", "i64.add", "i64.const", "i64.load",
                  "i64.store", "if", "local.get", "local.set", "local.tee", "loop", "memory.copy", "memory.fill", "memory.grow",
                  "memory.init", "memory.size", "nop", "ref.func", "ref.is_null", "ref.null", "return", "select", "table.copy", "table.get",
                  "table.grow", "table.init", "table.set", "throw", "try", "unreachable");
constexpr auto k_verilog_keywords = make_word_set(
    "always", "always_comb", "always_ff", "always_latch", "assign", "automatic", "begin", "case", "class", "clocking", "constraint",
    "covergroup", "default", "else", "end", "endcase", "endclass", "endfunction", "endgenerate", "endmodule", "endpackage", "endtask",
    "enum", "for", "foreach", "forever", "function", "generate", "genvar", "if", "initial", "interface", "localparam", "module", "package",
    "parameter", "program", "property", "repeat", "return", "sequence", "struct", "task", "typedef", "union", "while");
constexpr auto k_verilog_types =
    make_word_set("bit", "byte", "chandle", "event", "int", "integer", "logic", "longint", "real", "realtime", "reg", "shortint",
                  "shortreal", "signed", "string", "time", "unsigned", "var", "void", "wire");
constexpr auto k_vhdl_keywords = make_word_set(
    "architecture", "array", "assert", "attribute", "begin", "block", "body", "buffer", "bus", "case", "component", "configuration",
    "constant", "disconnect", "downto", "else", "elsif", "end", "entity", "file", "for", "function", "generate", "generic", "if", "impure",
    "in", "inertial", "inout", "is", "library", "linkage", "loop", "map", "new", "next", "of", "on", "open", "others", "out", "package",
    "port", "procedure", "process", "pure", "range", "record", "register", "reject", "report", "return", "select", "severity", "signal",
    "subtype", "then", "to", "transport", "type", "units", "use", "variable", "wait", "when", "while", "with");
constexpr auto k_vhdl_types = make_word_set("bit", "bit_vector", "boolean", "character", "integer", "natural", "positive", "real", "signed",
                                            "std_logic", "std_logic_vector", "string", "time", "unsigned");
constexpr auto k_winhex_commands = make_word_set("assign", "block", "close", "create", "delete", "exit", "find", "goto", "if", "messagebox",
                                                 "move", "open", "read", "replace", "save", "write");

constexpr u32 k_state_mask = 0xf000'0000;
constexpr u32 k_payload_mask = 0x0fff'ffff;
constexpr u32 k_normal = 0;
constexpr u32 k_block_comment = 0x1000'0000;
constexpr u32 k_nested_comment = 0x2000'0000;

enum struct Pending : u8 {
    NONE,
    MODULE,
    FUNCTION,
    TYPE,
};

[[nodiscard]] usize content_end(std::string_view source, usize begin, usize end) noexcept {
    if (end > begin && source[end - 1] == '\n') --end;
    if (end > begin && source[end - 1] == '\r') --end;
    return end;
}

[[nodiscard]] usize skip_space(std::string_view source, usize position, usize end) noexcept {
    while (position < end && (source[position] == ' ' || source[position] == '\t')) ++position;
    return position;
}

[[nodiscard]] bool equal_ascii_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index])) return false;
    }
    return true;
}

template <usize Size>
[[nodiscard]] bool contains_ascii_ci(const WordSet<Size> &set, std::string_view word) noexcept {
    return std::ranges::any_of(set.words, [&](std::string_view candidate) { return equal_ascii_ci(candidate, word); });
}

[[nodiscard]] bool identifier_start(char value) noexcept { return ascii_identifier_start(value) || value == '_' || value == '.'; }

[[nodiscard]] bool identifier_continue(char value) noexcept {
    return ascii_identifier_continue(value) || value == '_' || value == '.' || value == '-' || value == '$';
}

[[nodiscard]] usize word_end(std::string_view source, usize position, usize end) noexcept {
    while (position < end && identifier_continue(source[position])) ++position;
    return position;
}

[[nodiscard]] bool operator_character(char value) noexcept { return std::string_view("{}[]()<>;:,.@?~!%^&*+-=/|\\").contains(value); }

[[nodiscard]] usize string_end(std::string_view source, usize position, usize end, char quote) noexcept {
    while (position < end) {
        if (source[position] == '\\' && position + 1 < end)
            position += 2;
        else if (source[position++] == quote)
            return position;
    }
    return end;
}

void paint_escapes(LexContext &context, usize begin, usize end) {
    for (usize position = begin; position < end;) {
        if (context.source[position] == '\\' && position + 1 < end) {
            paint(context, {.begin = position, .end = position + 2}, Style::ESCAPE);
            position += 2;
        } else {
            ++position;
        }
    }
}

[[nodiscard]] bool line_comment(std::string_view source, usize position, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::ASSEMBLY: return source[position] == ';';
        case Dialect::CIL: return source.substr(position, 2) == "//";
        case Dialect::LLVM_IR: return source[position] == ';';
        case Dialect::SMALI: return source[position] == '#';
        case Dialect::WEBASSEMBLY: return source.substr(position, 2) == ";;";
        case Dialect::VERILOG: return source.substr(position, 2) == "//";
        case Dialect::VHDL: return source.substr(position, 2) == "--";
        case Dialect::WINHEX: return source.substr(position, 2) == "//";
    }
    return false;
}

[[nodiscard]] bool c_block_comments(Dialect dialect) noexcept {
    return dialect == Dialect::CIL || dialect == Dialect::VERILOG || dialect == Dialect::WINHEX;
}

[[nodiscard]] Style classify(std::string_view word, Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::ASSEMBLY:
            if (contains_ascii_ci(k_asm_instructions, word)) return Style::INSTRUCTION;
            if (contains_ascii_ci(k_asm_registers, word)) return Style::REGISTER;
            break;
        case Dialect::CIL:
            if (k_cil_types.contains(word)) return Style::TYPE;
            if (k_cil_keywords.contains(word)) return Style::KEYWORD;
            break;
        case Dialect::LLVM_IR:
            if (k_ir_types.contains(word)) return Style::TYPE;
            if (k_ir_instructions.contains(word)) return Style::INSTRUCTION;
            if (k_ir_keywords.contains(word)) return Style::KEYWORD;
            break;
        case Dialect::SMALI:
            if (k_smali_instructions.contains(word)) return Style::INSTRUCTION;
            if ((word.size() > 1 && (word[0] == 'v' || word[0] == 'p') && ascii_digit(word[1])) || word == "result") return Style::REGISTER;
            break;
        case Dialect::WEBASSEMBLY:
            if (k_wasm_instructions.contains(word)) return Style::INSTRUCTION;
            if (k_wasm_keywords.contains(word)) return Style::KEYWORD;
            if (word == "externref" || word == "f32" || word == "f64" || word == "funcref" || word == "i32" || word == "i64" ||
                word == "v128")
                return Style::TYPE;
            break;
        case Dialect::VERILOG:
            if (k_verilog_types.contains(word)) return Style::TYPE;
            if (k_verilog_keywords.contains(word)) return Style::KEYWORD;
            break;
        case Dialect::VHDL:
            if (contains_ascii_ci(k_vhdl_types, word)) return Style::TYPE;
            if (contains_ascii_ci(k_vhdl_keywords, word)) return Style::KEYWORD;
            break;
        case Dialect::WINHEX:
            if (contains_ascii_ci(k_winhex_commands, word)) return Style::INSTRUCTION;
            break;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] Pending pending_after(std::string_view word, Dialect dialect) noexcept {
    if (dialect == Dialect::VERILOG) {
        if (word == "module" || word == "package" || word == "interface") return Pending::MODULE;
        if (word == "function" || word == "task") return Pending::FUNCTION;
        if (word == "class" || word == "typedef") return Pending::TYPE;
    } else if (dialect == Dialect::VHDL) {
        if (equal_ascii_ci(word, "architecture") || equal_ascii_ci(word, "entity") || equal_ascii_ci(word, "library") ||
            equal_ascii_ci(word, "package"))
            return Pending::MODULE;
        if (equal_ascii_ci(word, "function") || equal_ascii_ci(word, "procedure")) return Pending::FUNCTION;
        if (equal_ascii_ci(word, "subtype") || equal_ascii_ci(word, "type")) return Pending::TYPE;
    } else if (dialect == Dialect::LLVM_IR && (word == "declare" || word == "define")) {
        return Pending::FUNCTION;
    }
    return Pending::NONE;
}

[[nodiscard]] Style pending_style(Pending pending) noexcept {
    switch (pending) {
        case Pending::MODULE: return Style::MODULE;
        case Pending::FUNCTION: return Style::FUNCTION;
        case Pending::TYPE: return Style::TYPE;
        case Pending::NONE: return Style::IDENTIFIER;
    }
    return Style::IDENTIFIER;
}

[[nodiscard]] u32 continue_comment(LexContext &context, usize &position, usize line_end, u32 state) {
    if ((state & k_state_mask) == k_block_comment) {
        const usize close = context.source.find("*/", position);
        const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + 2;
        paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
        position = token_end;
        return close == std::string_view::npos || close >= line_end ? state : k_normal;
    }
    u32 depth = std::max<u32>(1, state & k_payload_mask);
    const usize token_begin = position;
    while (position < line_end && depth != 0) {
        if (context.source.substr(position, 2) == "(;") {
            ++depth;
            position += 2;
        } else if (context.source.substr(position, 2) == ";)") {
            --depth;
            position += 2;
        } else {
            ++position;
        }
    }
    paint(context, {.begin = token_begin, .end = position}, Style::COMMENT);
    return depth == 0 ? k_normal : k_nested_comment | depth;
}

[[nodiscard]] u32 lex_line(LexContext &context, usize begin, usize end, u32 state, Dialect dialect) {
    const usize line_end = content_end(context.source, begin, end);
    usize position = begin;
    if ((state & k_state_mask) != k_normal) {
        state = continue_comment(context, position, line_end, state);
        if (state != k_normal) {
            if (end > line_end) paint(context, {.begin = line_end, .end = end}, Style::COMMENT);
            return state;
        }
    }

    Pending pending = Pending::NONE;
    while (position < line_end) {
        const char current = context.source[position];
        if (ascii_space(current)) {
            ++position;
            continue;
        }
        if (line_comment(context.source, position, dialect)) {
            paint(context, {.begin = position, .end = line_end}, Style::COMMENT);
            break;
        }
        if (c_block_comments(dialect) && context.source.substr(position, 2) == "/*") {
            const usize close = context.source.find("*/", position + 2);
            const usize token_end = close == std::string_view::npos || close >= line_end ? line_end : close + 2;
            paint(context, {.begin = position, .end = token_end}, Style::COMMENT);
            position = token_end;
            if (close == std::string_view::npos || close >= line_end) return k_block_comment;
            continue;
        }
        if (dialect == Dialect::WEBASSEMBLY && context.source.substr(position, 2) == "(;") {
            position += 2;
            state = continue_comment(context, position, line_end, k_nested_comment | 1);
            if (state != k_normal) return state;
            continue;
        }
        if (current == '"' || (current == '\'' && dialect != Dialect::VHDL)) {
            const usize token_begin = position++;
            position = string_end(context.source, position, line_end, current);
            paint(context, {.begin = token_begin, .end = position}, Style::STRING);
            paint_escapes(context, token_begin, position);
            continue;
        }
        if (dialect == Dialect::VERILOG && current == '`') {
            const usize token_begin = position++;
            position = word_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::DIRECTIVE);
            continue;
        }
        if (dialect == Dialect::SMALI && current == ':') {
            const usize token_begin = position++;
            position = word_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::LABEL);
            continue;
        }
        if (current == '.' && (dialect == Dialect::ASSEMBLY || dialect == Dialect::CIL || dialect == Dialect::SMALI)) {
            const usize token_begin = position++;
            position = word_end(context.source, position, line_end);
            paint(context, {.begin = token_begin, .end = position}, Style::DIRECTIVE);
            continue;
        }
        if ((dialect == Dialect::LLVM_IR && (current == '%' || current == '@' || current == '!')) ||
            (dialect == Dialect::WEBASSEMBLY && current == '$') || (dialect == Dialect::VERILOG && current == '$')) {
            const usize token_begin = position++;
            position = word_end(context.source, position, line_end);
            Style style = Style::PROPERTY;
            if (dialect == Dialect::LLVM_IR) {
                style = current == '@' ? (pending == Pending::FUNCTION ? Style::FUNCTION : Style::MODULE) :
                        current == '!' ? Style::ATTRIBUTE :
                                         Style::REGISTER;
            } else if (dialect == Dialect::WEBASSEMBLY)
                style = Style::REGISTER;
            else
                style = Style::FUNCTION;
            paint(context, {.begin = token_begin, .end = position}, style);
            if (current == '@') pending = Pending::NONE;
            continue;
        }
        if (ascii_digit(current) || (current == '-' && position + 1 < line_end && ascii_digit(context.source[position + 1]))) {
            const usize token_begin = position++;
            while (position < line_end &&
                   (ascii_alphanumeric(context.source[position]) || std::string_view("_.'+-").contains(context.source[position])))
                ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::NUMBER);
            continue;
        }
        if (identifier_start(current)) {
            const usize token_begin = position++;
            position = word_end(context.source, position, line_end);
            const std::string_view word = context.source.substr(token_begin, position - token_begin);
            Style style = classify(word, dialect);
            if (pending != Pending::NONE && style == Style::IDENTIFIER) {
                style = pending_style(pending);
                pending = Pending::NONE;
            }
            if (style == Style::IDENTIFIER) {
                const usize next = skip_space(context.source, position, line_end);
                if (next < line_end && context.source[next] == ':' && dialect == Dialect::ASSEMBLY)
                    style = Style::LABEL;
                else if (next < line_end && context.source[next] == '(' && dialect == Dialect::WINHEX)
                    style = Style::FUNCTION;
            }
            if (style == Style::KEYWORD) pending = pending_after(word, dialect);
            paint(context, {.begin = token_begin, .end = position}, style);
            continue;
        }
        if (operator_character(current)) {
            const usize token_begin = position++;
            while (position < line_end && operator_character(context.source[position])) ++position;
            paint(context, {.begin = token_begin, .end = position}, Style::OPERATOR);
            continue;
        }
        ++position;
    }
    return k_normal;
}

} // namespace

void AssemblyLexer::lex(LexContext &context) const {
    const auto first_next = std::ranges::upper_bound(context.line_starts, context.range.begin);
    usize line = static_cast<usize>(first_next - context.line_starts.begin() - 1);
    contract_assert(context.line_starts[line] == context.range.begin);
    while (line < context.line_starts.size()) {
        const usize begin = context.line_starts[line];
        const usize end =
            std::min(context.range.end, line + 1 < context.line_starts.size() ? context.line_starts[line + 1] : context.source.size());
        const u32 next_state = lex_line(context, begin, end, context.line_states[line], dialect);
        if (line + 1 < context.line_states.size() && end == context.line_starts[line + 1]) context.line_states[line + 1] = next_state;
        if (end >= context.range.end) break;
        ++line;
    }
}

} // namespace lighter::lexer
