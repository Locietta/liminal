#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <lighter/async/io/terminal.h>

namespace lighter::detail {

/// Stateful decoder for POSIX VT input bytes. Input may be fed at arbitrary
/// chunk boundaries; emitted events never contain partial UTF-8 sequences.
struct TerminalInputDecoder {
    void feed(std::string_view bytes, std::function_ref<void(TerminalEvent)> emit);

    /// Resolves an incomplete escape prefix after the terminal's escape-key
    /// disambiguation timeout expires.
    void flush_escape(std::function_ref<void(TerminalEvent)> emit);

    bool escape_pending() const noexcept;

private:
    void parse(bool flush_escape, std::function_ref<void(TerminalEvent)> emit);

    std::string input;
    std::string paste;
    bool pasting = false;
};

} // namespace lighter::detail
