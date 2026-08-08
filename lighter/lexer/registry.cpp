#include "registry.h"

#include <array>
#include <span>

#include <lighter/lexer/ascii.h>
#include <lighter/lexer/language/cpp.h>
#include <lighter/lexer/language/python.h>
#include <lighter/lexer/language/rust.h>

namespace lighter::lexer {

namespace {

using namespace std::literals::string_view_literals;

using Factory = Lexer (*)();

struct LanguageBinding {
    std::span<const std::string_view> names;
    Factory create;
};

constexpr std::array k_cpp_names = {
    "c"sv, "c++"sv, "cc"sv, "cpp"sv, "cxx"sv, "h"sv, "hh"sv, "hpp"sv, "hxx"sv, "idl"sv, "midl"sv, "odl"sv,
};
constexpr std::array k_objective_names = {
    "m"sv, "mm"sv, "obj-c"sv, "obj-c++"sv, "objc"sv, "objc++"sv, "objcpp"sv, "objective-c"sv, "objective-c++"sv, "objective-cpp"sv,
};
constexpr std::array k_resource_names = {"dlg"sv, "rc"sv, "rc2"sv, "rct"sv, "resource"sv, "resource-script"sv, "rh"sv};
constexpr std::array k_rust_names = {"rs"sv, "rust"sv};
constexpr std::array k_python_names = {"py"sv, "python"sv, "python3"sv};

template <auto ConfiguredLexer>
[[nodiscard]] Lexer create_configured_lexer() {
    return make_lexer(ConfiguredLexer);
}

constexpr std::array k_registry = {
    LanguageBinding{.names = k_cpp_names, .create = create_configured_lexer<CppLexer{.dialect = CppDialect::CPP}>},
    LanguageBinding{.names = k_objective_names, .create = create_configured_lexer<CppLexer{.dialect = CppDialect::OBJECTIVE_C}>},
    LanguageBinding{.names = k_resource_names, .create = create_configured_lexer<CppLexer{.dialect = CppDialect::RESOURCE_SCRIPT}>},
    LanguageBinding{.names = k_rust_names, .create = create_configured_lexer<RustLexer{}>},
    LanguageBinding{.names = k_python_names, .create = create_configured_lexer<PythonLexer{}>},
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
    for (const LanguageBinding &binding : k_registry) {
        for (std::string_view candidate : binding.names) {
            if (equal_ignoring_ascii_case(name, candidate)) return binding.create();
        }
    }
    return std::nullopt;
}

} // namespace lighter::lexer
