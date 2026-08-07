#include "syntax_highlight.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace liminal::tui {

namespace {

constexpr usize k_max_highlight_bytes = 512 * 1024;
constexpr usize k_max_highlight_lines = 10'000;
constexpr usize k_max_highlight_line_bytes = 4 * 1024;

constexpr std::string_view k_c_like_keywords[] = {
    "alignas",
    "alignof",
    "and",
    "asm",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "char8_t",
    "char16_t",
    "char32_t",
    "class",
    "compl",
    "concept",
    "const",
    "consteval",
    "constexpr",
    "constinit",
    "const_cast",
    "continue",
    "co_await",
    "co_return",
    "co_yield",
    "decltype",
    "default",
    "delete",
    "do",
    "double",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "final",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "noexcept",
    "not",
    "not_eq",
    "nullptr",
    "operator",
    "or",
    "or_eq",
    "override",
    "private",
    "protected",
    "public",
    "register",
    "reinterpret_cast",
    "requires",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "static_cast",
    "struct",
    "switch",
    "template",
    "this",
    "thread_local",
    "throw",
    "true",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "wchar_t",
    "while",
};

constexpr std::string_view k_rust_keywords[] = {
    "as",     "async", "await", "become",   "box",    "break",  "const",   "continue", "crate",   "do",    "dyn",    "else",   "enum",
    "extern", "false", "final", "fn",       "for",    "gen",    "if",      "impl",     "in",      "let",   "loop",   "macro",  "match",
    "mod",    "move",  "mut",   "override", "priv",   "pub",    "ref",     "return",   "self",    "Self",  "static", "struct", "super",
    "trait",  "true",  "try",   "type",     "typeof", "unsafe", "unsized", "use",      "virtual", "where", "while",  "yield",
};

constexpr std::string_view k_javascript_keywords[] = {
    "as",      "async",    "await", "break",   "case",       "catch",     "class",   "const",      "continue",  "debugger",
    "default", "delete",   "do",    "else",    "enum",       "export",    "extends", "false",      "finally",   "for",
    "from",    "function", "get",   "if",      "implements", "import",    "in",      "instanceof", "interface", "let",
    "new",     "null",     "of",    "package", "private",    "protected", "public",  "return",     "set",       "static",
    "super",   "switch",   "this",  "throw",   "true",       "try",       "typeof",  "undefined",
};

constexpr std::string_view k_python_keywords[] = {
    "False", "None",     "True", "and",    "as",      "assert", "async",  "await",  "break", "case",   "class", "continue", "def",
    "del",   "elif",     "else", "except", "finally", "for",    "from",   "global", "if",    "import", "in",    "is",       "lambda",
    "match", "nonlocal", "not",  "or",     "pass",    "raise",  "return", "try",    "while", "with",   "yield", "type",
};

constexpr std::string_view k_shell_keywords[] = {
    "case", "coproc", "do",   "done", "elif",  "else",  "esac",    "fi",    "for",      "function", "if",
    "in",   "select", "then", "time", "until", "while", "declare", "local", "readonly", "return",   "typeset",
};

constexpr std::string_view k_go_keywords[] = {
    "break", "case",   "chan",      "const", "continue", "default", "defer",  "else",   "fallthrough", "for",    "func", "go",  "goto",
    "if",    "import", "interface", "map",   "package",  "range",   "return", "select", "struct",      "switch", "type", "var",
};

constexpr std::string_view k_sql_keywords[] = {
    "all",    "alter",      "and",     "as",         "asc",     "begin",    "between", "by",       "case",  "check", "column",
    "commit", "constraint", "create",  "database",   "default", "delete",   "desc",    "distinct", "drop",  "else",  "end",
    "exists", "false",      "foreign", "from",       "full",    "group",    "having",  "in",       "index", "inner", "insert",
    "into",   "is",         "join",    "key",        "left",    "like",     "limit",   "not",      "null",  "on",    "or",
    "order",  "outer",      "primary", "references", "right",   "rollback", "select",  "set",      "table", "true",  "union",
};

constexpr std::string_view k_data_keywords[] = {
    "false", "null", "true", "False", "None", "True", "no", "off", "on", "yes", "inf", "nan",
};

constexpr std::string_view k_c_like_types[] = {
    "auto",    "bool",     "byte",     "char",     "char8_t", "char16_t", "char32_t", "decimal", "double", "dynamic", "float",   "int",
    "int8_t",  "int16_t",  "int32_t",  "int64_t",  "long",    "object",   "sbyte",    "short",   "signed", "size_t",  "ssize_t", "string",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "ulong",   "unsigned", "ushort",   "usize",   "void",   "wchar_t", "String",  "Object",
};

constexpr std::string_view k_rust_types[] = {
    "bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize", "str", "u8", "u16", "u32", "u64", "u128", "usize", "Self",
};

constexpr std::string_view k_javascript_types[] = {
    "any", "bigint", "boolean", "never", "number", "object", "string", "symbol", "unknown", "void",
};

constexpr std::string_view k_python_types[] = {
    "bool", "bytearray",  "bytes",  "complex", "dict", "float", "frozenset", "int",
    "list", "memoryview", "object", "range",   "set",  "str",   "tuple",     "type",
};

constexpr std::string_view k_go_types[] = {
    "any",   "bool",  "byte", "complex64", "complex128", "error", "float32", "float64", "int",    "int8",    "int16",
    "int32", "int64", "rune", "string",    "uint",       "uint8", "uint16",  "uint32",  "uint64", "uintptr",
};

constexpr std::string_view k_sql_types[] = {
    "bigint",  "binary", "blob",    "boolean", "char",     "date", "decimal", "double",    "float",   "int",
    "integer", "json",   "numeric", "real",    "smallint", "text", "time",    "timestamp", "varchar",
};

constexpr std::string_view k_c_like_constants[] = {"false", "nullptr", "null", "true"};
constexpr std::string_view k_rust_constants[] = {"false", "None", "true"};
constexpr std::string_view k_javascript_constants[] = {"false", "Infinity", "NaN", "null", "true", "undefined"};
constexpr std::string_view k_python_constants[] = {"False", "None", "NotImplemented", "True", "Ellipsis"};
constexpr std::string_view k_go_constants[] = {"false", "iota", "nil", "true"};
constexpr std::string_view k_sql_constants[] = {"false", "null", "true"};

void append_span(std::vector<StyledSpan> &spans, std::string_view text, Style style) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
        spans.back().text += text;
    } else {
        spans.push_back({.text = std::string(text), .style = style});
    }
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

CodeLanguage resolve_language(std::string_view language) {
    const auto name = lowercase(language);
    if (name == "c" || name == "h" || name == "cc" || name == "cpp" || name == "c++" || name == "cxx" || name == "hpp" ||
        name == "csharp" || name == "c#" || name == "java" || name == "kotlin" || name == "kt" || name == "swift" || name == "dart" ||
        name == "objective-c" || name == "objc") {
        return CodeLanguage::C_LIKE;
    }
    if (name == "rust" || name == "rs") return CodeLanguage::RUST;
    if (name == "javascript" || name == "js" || name == "jsx" || name == "typescript" || name == "ts" || name == "tsx") {
        return CodeLanguage::JAVASCRIPT;
    }
    if (name == "python" || name == "python3" || name == "py") return CodeLanguage::PYTHON;
    if (name == "bash" || name == "sh" || name == "shell" || name == "zsh" || name == "fish" || name == "powershell" || name == "pwsh" ||
        name == "ps1") {
        return CodeLanguage::SHELL;
    }
    if (name == "go" || name == "golang") return CodeLanguage::GO;
    if (name == "sql" || name == "mysql" || name == "postgresql" || name == "sqlite") return CodeLanguage::SQL;
    if (name == "json" || name == "jsonc") return CodeLanguage::DATA;
    if (name == "yaml" || name == "yml" || name == "toml") return CodeLanguage::CONFIG;
    return CodeLanguage::PLAIN;
}

template <usize Size>
bool contains_keyword(const std::string_view (&keywords)[Size], std::string_view word) {
    return std::ranges::find(keywords, word) != keywords + Size;
}

bool keyword(CodeLanguage language, std::string_view word) {
    switch (language) {
        case CodeLanguage::C_LIKE: return contains_keyword(k_c_like_keywords, word);
        case CodeLanguage::RUST: return contains_keyword(k_rust_keywords, word);
        case CodeLanguage::JAVASCRIPT: return contains_keyword(k_javascript_keywords, word);
        case CodeLanguage::PYTHON: return contains_keyword(k_python_keywords, word);
        case CodeLanguage::SHELL: return contains_keyword(k_shell_keywords, word);
        case CodeLanguage::GO: return contains_keyword(k_go_keywords, word);
        case CodeLanguage::SQL: return contains_keyword(k_sql_keywords, lowercase(word));
        case CodeLanguage::DATA:
        case CodeLanguage::CONFIG: return contains_keyword(k_data_keywords, word);
        case CodeLanguage::PLAIN: return false;
    }
    return false;
}

bool type_keyword(CodeLanguage language, std::string_view word) {
    switch (language) {
        case CodeLanguage::C_LIKE: return contains_keyword(k_c_like_types, word);
        case CodeLanguage::RUST: return contains_keyword(k_rust_types, word);
        case CodeLanguage::JAVASCRIPT: return contains_keyword(k_javascript_types, word);
        case CodeLanguage::PYTHON: return contains_keyword(k_python_types, word);
        case CodeLanguage::GO: return contains_keyword(k_go_types, word);
        case CodeLanguage::SQL: return contains_keyword(k_sql_types, lowercase(word));
        case CodeLanguage::PLAIN:
        case CodeLanguage::SHELL:
        case CodeLanguage::DATA:
        case CodeLanguage::CONFIG: return false;
    }
    return false;
}

bool constant_keyword(CodeLanguage language, std::string_view word) {
    switch (language) {
        case CodeLanguage::C_LIKE: return contains_keyword(k_c_like_constants, word);
        case CodeLanguage::RUST: return contains_keyword(k_rust_constants, word);
        case CodeLanguage::JAVASCRIPT: return contains_keyword(k_javascript_constants, word);
        case CodeLanguage::PYTHON: return contains_keyword(k_python_constants, word);
        case CodeLanguage::GO: return contains_keyword(k_go_constants, word);
        case CodeLanguage::SQL: return contains_keyword(k_sql_constants, lowercase(word));
        case CodeLanguage::DATA:
        case CodeLanguage::CONFIG: return contains_keyword(k_data_keywords, word);
        case CodeLanguage::PLAIN:
        case CodeLanguage::SHELL: return false;
    }
    return false;
}

bool identifier_start(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalpha(byte) != 0 || character == '_' || character == '$';
}

bool identifier_continue(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_' || character == '$';
}

bool slash_comments(CodeLanguage language) {
    return language == CodeLanguage::C_LIKE || language == CodeLanguage::RUST || language == CodeLanguage::JAVASCRIPT ||
           language == CodeLanguage::GO;
}

bool hash_comments(CodeLanguage language) {
    return language == CodeLanguage::PYTHON || language == CodeLanguage::SHELL || language == CodeLanguage::CONFIG;
}

bool block_comments(CodeLanguage language) { return slash_comments(language) || language == CodeLanguage::SQL; }

bool starts_at(std::string_view line, usize offset, std::string_view token) { return line.substr(offset).starts_with(token); }

usize next_non_space(std::string_view line, usize offset) {
    while (offset < line.size() && std::isspace(static_cast<unsigned char>(line[offset])) != 0) ++offset;
    return offset;
}

bool member_access_before(std::string_view line, usize offset) {
    while (offset > 0 && std::isspace(static_cast<unsigned char>(line[offset - 1])) != 0) --offset;
    if (offset > 0 && line[offset - 1] == '.') return true;
    return offset >= 2 && line.substr(offset - 2, 2) == "->";
}

bool constant_name(std::string_view word) {
    usize letters = 0;
    for (const auto character : word) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalpha(byte) != 0) {
            if (std::islower(byte) != 0) return false;
            ++letters;
        } else if (std::isdigit(byte) == 0 && character != '_') {
            return false;
        }
    }
    return letters >= 2;
}

bool inferred_type_name(CodeLanguage language, std::string_view word) {
    if (language == CodeLanguage::PLAIN || language == CodeLanguage::SHELL || language == CodeLanguage::SQL ||
        language == CodeLanguage::DATA || language == CodeLanguage::CONFIG || word.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(word.front());
    return std::isupper(first) != 0 && !constant_name(word);
}

bool property_key(CodeLanguage language, std::string_view line, usize offset) {
    if (language != CodeLanguage::DATA && language != CodeLanguage::CONFIG) return false;
    offset = next_non_space(line, offset);
    return offset < line.size() && (line[offset] == ':' || (language == CodeLanguage::CONFIG && line[offset] == '='));
}

bool operator_character(char character) { return std::string_view("+-*/%=!<>?:&|^~").contains(character); }

struct QuotedEnd {
    usize offset = 0;
    bool closed = false;
};

QuotedEnd quoted_end(std::string_view line, usize offset, char quote) {
    ++offset;
    bool escaped = false;
    while (offset < line.size()) {
        const auto character = line[offset++];
        if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == quote) {
            return {.offset = offset, .closed = true};
        }
    }
    return {.offset = offset};
}

usize number_end(std::string_view line, usize offset) {
    while (offset < line.size()) {
        const auto character = line[offset];
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) == 0 && character != '.' && character != '_' && character != '\'') break;
        ++offset;
    }
    return offset;
}

std::vector<StyledSpan> fallback(std::string_view line) {
    if (line.empty()) return {};
    return {{.text = std::string(line), .style = Style::CODE}};
}

} // namespace

CodeHighlighter::CodeHighlighter(std::string_view language) : language(resolve_language(language)) {}

std::vector<StyledSpan> CodeHighlighter::highlight_line(std::string_view line) {
    total_bytes += line.size();
    ++line_count;
    if (line.size() > k_max_highlight_line_bytes || total_bytes > k_max_highlight_bytes || line_count > k_max_highlight_lines) {
        enabled = false;
    }
    if (!supported()) return fallback(line);

    std::vector<StyledSpan> spans;
    usize offset = 0;
    while (offset < line.size()) {
        if (block_comment) {
            const auto end = line.find("*/", offset);
            const auto next = end == std::string_view::npos ? line.size() : end + 2;
            append_span(spans, line.substr(offset, next - offset), Style::CODE_COMMENT);
            offset = next;
            if (end != std::string_view::npos) block_comment = false;
            continue;
        }

        if (multiline_quote != 0) {
            const auto delimiter = multiline_triple ? std::string(3, multiline_quote) : std::string(1, multiline_quote);
            const auto end = line.find(delimiter, offset);
            const auto next = end == std::string_view::npos ? line.size() : end + delimiter.size();
            append_span(spans, line.substr(offset, next - offset), Style::CODE_STRING);
            offset = next;
            if (end != std::string_view::npos) {
                multiline_quote = 0;
                multiline_triple = false;
            }
            continue;
        }

        if (slash_comments(language) && starts_at(line, offset, "//")) {
            append_span(spans, line.substr(offset), Style::CODE_COMMENT);
            break;
        }
        if (language == CodeLanguage::SQL && starts_at(line, offset, "--")) {
            append_span(spans, line.substr(offset), Style::CODE_COMMENT);
            break;
        }
        if (hash_comments(language) && line[offset] == '#' &&
            (language != CodeLanguage::SHELL || offset == 0 || std::isspace(static_cast<unsigned char>(line[offset - 1])) != 0)) {
            append_span(spans, line.substr(offset), Style::CODE_COMMENT);
            break;
        }
        if (block_comments(language) && starts_at(line, offset, "/*")) {
            const auto end = line.find("*/", offset + 2);
            const auto next = end == std::string_view::npos ? line.size() : end + 2;
            append_span(spans, line.substr(offset, next - offset), Style::CODE_COMMENT);
            offset = next;
            block_comment = end == std::string_view::npos;
            continue;
        }

        if (language == CodeLanguage::PYTHON && (starts_at(line, offset, "\"\"\"") || starts_at(line, offset, "'''"))) {
            const auto quote = line[offset];
            const auto end = line.find(std::string(3, quote), offset + 3);
            const auto next = end == std::string_view::npos ? line.size() : end + 3;
            append_span(spans, line.substr(offset, next - offset), Style::CODE_STRING);
            offset = next;
            if (end == std::string_view::npos) {
                multiline_quote = quote;
                multiline_triple = true;
            }
            continue;
        }

        const auto character = line[offset];
        const bool string_quote = character == '"' || character == '\'' || (language == CodeLanguage::JAVASCRIPT && character == '`');
        if (string_quote) {
            const auto end = quoted_end(line, offset, character);
            const auto key = end.closed && property_key(language, line, end.offset);
            append_span(spans, line.substr(offset, end.offset - offset), key ? Style::CODE_PROPERTY : Style::CODE_STRING);
            if (character == '`' && !end.closed) {
                multiline_quote = '`';
                multiline_triple = false;
            }
            offset = end.offset;
            continue;
        }

        if (language == CodeLanguage::C_LIKE && character == '#' &&
            line.substr(0, offset).find_first_not_of(" \t") == std::string_view::npos) {
            auto next = offset + 1;
            while (next < line.size() && identifier_continue(line[next])) ++next;
            append_span(spans, line.substr(offset, next - offset), Style::CODE_KEYWORD);
            offset = next;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(character)) != 0 ||
            (character == '.' && offset + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[offset + 1])) != 0)) {
            const auto next = number_end(line, offset + 1);
            append_span(spans, line.substr(offset, next - offset), Style::CODE_NUMBER);
            offset = next;
            continue;
        }

        if (identifier_start(character)) {
            auto next = offset + 1;
            while (next < line.size() && identifier_continue(line[next])) ++next;
            const auto word = line.substr(offset, next - offset);
            auto style = Style::NORMAL;
            if (constant_keyword(language, word) || constant_name(word)) {
                style = Style::CODE_CONSTANT;
            } else if (type_keyword(language, word) || inferred_type_name(language, word)) {
                style = Style::CODE_TYPE;
            } else if (keyword(language, word)) {
                style = Style::CODE_KEYWORD;
            } else if (character == '$' && (language == CodeLanguage::SHELL || language == CodeLanguage::JAVASCRIPT)) {
                style = Style::CODE_PROPERTY;
            } else if (const auto following = next_non_space(line, next); following < line.size() && line[following] == '(') {
                style = Style::CODE_FUNCTION;
            } else if (member_access_before(line, offset) || property_key(language, line, next)) {
                style = Style::CODE_PROPERTY;
            }
            append_span(spans, word, style);
            offset = next;
            continue;
        }

        if (operator_character(character)) {
            auto next = offset + 1;
            while (next < line.size() && operator_character(line[next])) ++next;
            append_span(spans, line.substr(offset, next - offset), Style::CODE_OPERATOR);
            offset = next;
            continue;
        }

        auto next = offset + 1;
        while (next < line.size() && !identifier_start(line[next]) && std::isdigit(static_cast<unsigned char>(line[next])) == 0 &&
               line[next] != '"' && line[next] != '\'' && !operator_character(line[next]) &&
               !(language == CodeLanguage::JAVASCRIPT && line[next] == '`')) {
            if ((slash_comments(language) && (starts_at(line, next, "//") || starts_at(line, next, "/*"))) ||
                (language == CodeLanguage::SQL && (starts_at(line, next, "--") || starts_at(line, next, "/*"))) ||
                (hash_comments(language) && line[next] == '#')) {
                break;
            }
            ++next;
        }
        append_span(spans, line.substr(offset, next - offset), Style::NORMAL);
        offset = next;
    }
    return spans;
}

} // namespace liminal::tui
