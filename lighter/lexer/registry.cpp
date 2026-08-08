#include "registry.h"

#include <array>
#include <span>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/language/cpp.h>
#include <lighter/lexer/language/python.h>
#include <lighter/lexer/language/rust.h>

namespace lighter::lexer {

namespace {

using Factory = Lexer (*)();

struct RegistryEntry {
    std::span<const std::string_view> names;
    Factory create;
};

constexpr std::array k_cpp_names = {
    std::string_view{"c"},   std::string_view{"c++"}, std::string_view{"cc"},  std::string_view{"cpp"},
    std::string_view{"cxx"}, std::string_view{"h"},   std::string_view{"hpp"}, std::string_view{"hxx"},
};
constexpr std::array k_rust_names = {std::string_view{"rs"}, std::string_view{"rust"}};
constexpr std::array k_python_names = {std::string_view{"py"}, std::string_view{"python"}, std::string_view{"python3"}};

[[nodiscard]] Lexer create_cpp() { return make_lexer(CppLexer{}); }
[[nodiscard]] Lexer create_rust() { return make_lexer(RustLexer{}); }
[[nodiscard]] Lexer create_python() { return make_lexer(PythonLexer{}); }

constexpr std::array k_registry = {
    RegistryEntry{.names = k_cpp_names, .create = create_cpp},
    RegistryEntry{.names = k_rust_names, .create = create_rust},
    RegistryEntry{.names = k_python_names, .create = create_python},
};

[[nodiscard]] bool equal_ignoring_ascii_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index])) return false;
    }
    return true;
}

} // namespace

std::optional<Lexer> lexer_for_language(std::string_view name) {
    for (const RegistryEntry &entry : k_registry) {
        for (std::string_view candidate : entry.names) {
            if (equal_ignoring_ascii_case(name, candidate)) return entry.create();
        }
    }
    return std::nullopt;
}

} // namespace lighter::lexer
