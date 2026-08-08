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

struct LanguageBinding {
    std::span<const std::string_view> names;
    Factory create;
};

constexpr std::array k_cpp_names = {
    std::string_view{"c"},   std::string_view{"c++"}, std::string_view{"cc"},   std::string_view{"cpp"},
    std::string_view{"cxx"}, std::string_view{"h"},   std::string_view{"hh"},   std::string_view{"hpp"},
    std::string_view{"hxx"}, std::string_view{"idl"}, std::string_view{"midl"}, std::string_view{"odl"},
};
constexpr std::array k_objective_names = {
    std::string_view{"m"},
    std::string_view{"mm"},
    std::string_view{"obj-c"},
    std::string_view{"obj-c++"},
    std::string_view{"objc"},
    std::string_view{"objc++"},
    std::string_view{"objcpp"},
    std::string_view{"objective-c"},
    std::string_view{"objective-c++"},
    std::string_view{"objective-cpp"},
};
constexpr std::array k_resource_names = {std::string_view{"dlg"}, std::string_view{"rc"},       std::string_view{"rc2"},
                                         std::string_view{"rct"}, std::string_view{"resource"}, std::string_view{"resource-script"},
                                         std::string_view{"rh"}};
constexpr std::array k_rust_names = {std::string_view{"rs"}, std::string_view{"rust"}};
constexpr std::array k_python_names = {std::string_view{"py"}, std::string_view{"python"}, std::string_view{"python3"}};

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
