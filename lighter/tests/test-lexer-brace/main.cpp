#include <iostream>
#include <string_view>

#include <lighter/lexer/document.h>
#include <lighter/lexer/language/brace.h>
#include <lighter/lexer/role.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

[[nodiscard]] bool has_role(const Document &document, std::string_view token, TokenRole role, usize start = 0) {
    const usize position = document.source.find(token, start);
    if (position == std::string::npos) {
        std::cerr << "missing token: " << token << '\n';
        return false;
    }
    for (usize offset = position; offset < position + token.size(); ++offset) {
        const TokenRole actual = role_for_style<BraceLexer::Style>(document.styles[offset]);
        if (actual != role) {
            std::cerr << "role mismatch for " << token << ": expected " << static_cast<int>(role) << ", got " << static_cast<int>(actual)
                      << '\n';
            return false;
        }
    }
    return true;
}

void lex(Document &document, BraceDialect dialect, LexRange range) {
    auto lex_context = context(document, range);
    BraceLexer{.dialect = dialect}.lex(lex_context);
}

bool test_java_and_csharp() {
    Document java;
    assign(
        java,
        "package app;\n/** docs */\npublic record Server(String name) {\n  String render() { return String.format(\"%s\", name); }\n}\n");
    lex(java, BraceDialect::JAVA, {.begin = 0, .end = java.source.size()});

    Document csharp;
    assign(csharp,
           "#nullable enable\n[Obsolete]\npublic record Server(string Name) {\n  public string Render() => $@\"value={Name}\";\n}\n");
    lex(csharp, BraceDialect::CSHARP, {.begin = 0, .end = csharp.source.size()});
    return has_role(java, "package", TokenRole::KEYWORD) && has_role(java, "app", TokenRole::MODULE) &&
           has_role(java, "/** docs */", TokenRole::DOCUMENTATION) && has_role(java, "Server", TokenRole::TYPE) &&
           has_role(java, "render", TokenRole::FUNCTION) && has_role(java, "%s", TokenRole::ESCAPE) &&
           has_role(csharp, "#nullable enable", TokenRole::PREPROCESSOR) && has_role(csharp, "Obsolete", TokenRole::ATTRIBUTE) &&
           has_role(csharp, "Server", TokenRole::TYPE) && has_role(csharp, "string", TokenRole::TYPE) &&
           has_role(csharp, "Render", TokenRole::FUNCTION) && has_role(csharp, "{Name}", TokenRole::PARAMETER);
}

bool test_kotlin_swift_and_zig() {
    Document kotlin;
    assign(kotlin, "package app\ndata class User(val name: String)\nfun greet(user: User) = \"hello ${user.name}\"\n");
    lex(kotlin, BraceDialect::KOTLIN, {.begin = 0, .end = kotlin.source.size()});

    Document swift;
    assign(swift, "protocol Renderable { func render() -> String }\n/* outer\n  /* inner */\n*/\nstruct View: Renderable {}\n");
    lex(swift, BraceDialect::SWIFT, {.begin = 0, .end = swift.source.size()});

    Document zig;
    assign(zig, "const std = @import(\"std\");\npub fn main() void {\n  const text = \\\\multiline\n}\n");
    lex(zig, BraceDialect::ZIG, {.begin = 0, .end = zig.source.size()});
    return has_role(kotlin, "app", TokenRole::MODULE) && has_role(kotlin, "User", TokenRole::TYPE) &&
           has_role(kotlin, "greet", TokenRole::FUNCTION) && has_role(kotlin, "${user.name}", TokenRole::PARAMETER) &&
           has_role(swift, "Renderable", TokenRole::TYPE) && has_role(swift, "inner", TokenRole::COMMENT) &&
           has_role(swift, "View", TokenRole::TYPE) && has_role(zig, "std", TokenRole::PROPERTY) &&
           has_role(zig, "@import", TokenRole::ATTRIBUTE) && has_role(zig, "main", TokenRole::FUNCTION) &&
           has_role(zig, "\\\\multiline", TokenRole::STRING);
}

bool test_streamed_triple_string() {
    Document document;
    assign(document, "class Message {\n  String value = \"\"\"first\n");
    lex(document, BraceDialect::JAVA, {.begin = 0, .end = document.source.size()});
    const LexRange dirty = append(document, "second\"\"\";\n}\n");
    lex(document, BraceDialect::JAVA, dirty);
    return has_role(document, "first", TokenRole::STRING) && has_role(document, "second", TokenRole::STRING);
}

} // namespace

int main() { return test_java_and_csharp() && test_kotlin_swift_and_zig() && test_streamed_triple_string() ? 0 : 1; }
