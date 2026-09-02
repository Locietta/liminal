#pragma once

#include <source_location>
#include <string_view>

namespace lighter {

[[noreturn]] void panic(std::string_view message, std::source_location location = std::source_location::current()) noexcept;

using PanicHook = void (*)() noexcept;

/// Installs a process-wide hook that runs once, before the diagnostic is
/// printed, when the process panics, violates an enforced contract, or
/// terminates. It exists for last-chance cleanup that must precede the abort,
/// such as restoring a raw-mode terminal so the diagnostic is readable. A null
/// hook removes the current one. Returns the previous hook.
PanicHook set_panic_hook(PanicHook hook) noexcept;

inline void check(bool condition, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
    if (!condition) [[unlikely]] {
        lighter::panic(message, location);
    }
}

} // namespace lighter
