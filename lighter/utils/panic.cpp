#include "panic.h"

#include <contracts>
#include <cstdio>
#include <cstdlib>
#include <stacktrace>

#include <lighter/types.hpp>
#include <lighter/utils/config.h>

namespace lighter::detail {

void print_stacktrace(usize skip) noexcept {
#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#if LIGHTER_ENABLE_EXCEPTIONS
    try {
#endif
        const auto trace = std::stacktrace::current(skip);
        if (trace.empty()) {
            std::fputs("Stack trace unavailable.\n", stderr);
            return;
        }

        std::fputs("Stack trace:\n", stderr);
        usize index = 0;
        for (const auto &entry : trace) {
            const auto description = entry.description();
            const auto file = entry.source_file();
            std::fprintf(stderr, "  #%zu %s%s%s%s%u\n", index++, description.empty() ? "<unknown>" : description.c_str(),
                         file.empty() ? "" : " at ", file.empty() ? "" : file.c_str(), file.empty() ? "" : ":",
                         static_cast<unsigned>(entry.source_line()));
        }
#if LIGHTER_ENABLE_EXCEPTIONS
    } catch (...) {
        std::fputs("Stack trace unavailable while handling panic.\n", stderr);
    }
#endif
#else
    std::fputs("Stack trace unavailable in this standard library.\n", stderr);
#endif
}

void report_contract_violation(const std::contracts::contract_violation &violation) noexcept {
    const auto location = violation.location();
    const char *kind = "assert";
    switch (violation.kind()) {
        case std::contracts::assertion_kind::pre: kind = "precondition"; break;
        case std::contracts::assertion_kind::post: kind = "postcondition"; break;
        case std::contracts::assertion_kind::assert: break;
    }

    std::fprintf(stderr, "CONTRACT VIOLATION: %s: %s\n  at %s:%u in %s\n", kind, violation.comment(), location.file_name(),
                 static_cast<unsigned>(location.line()), location.function_name());
    print_stacktrace(3);
    std::fflush(stderr);
}

} // namespace lighter::detail

namespace lighter {

[[noreturn]] void panic(std::string_view message, std::source_location location) noexcept {
    std::fprintf(stderr, "PANIC: %.*s\n  at %s:%u in %s\n", static_cast<int>(message.size()), message.empty() ? "" : message.data(),
                 location.file_name(), static_cast<unsigned>(location.line()), location.function_name());
    detail::print_stacktrace(2);
    std::fflush(stderr);
    std::abort();
}

} // namespace lighter

void handle_contract_violation(const std::contracts::contract_violation &violation) noexcept {
    lighter::detail::report_contract_violation(violation);
}
