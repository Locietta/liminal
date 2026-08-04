#include "terminal_input_decoder.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <lighter/encoding/utf8.h>

namespace lighter::detail {

namespace {

constexpr std::string_view k_paste_start = "\x1b[200~";
constexpr std::string_view k_paste_end = "\x1b[201~";

struct DecodedEscape {
    TerminalKey key = TerminalKey::UNKNOWN;
    TerminalModifiers modifiers = TerminalModifiers::NONE;
    usize size = 0;
};

struct CsiParameters {
    std::array<i32, 2> values{};
    usize size = 0;
};

TerminalEvent key_event(TerminalKey key, TerminalModifiers modifiers = TerminalModifiers::NONE, std::string text = {}) {
    return TerminalEvent{.kind = TerminalEventKind::KEY, .key = key, .modifiers = modifiers, .text = std::move(text)};
}

TerminalEvent text_event(std::string text) { return TerminalEvent{.kind = TerminalEventKind::TEXT, .text = std::move(text)}; }

bool csi_final(u8 byte) noexcept { return byte >= 0x40 && byte <= 0x7e; }

std::optional<CsiParameters> parse_csi_parameters(std::string_view encoded) {
    CsiParameters result;
    if (encoded.empty()) return result;

    i32 value = 0;
    bool has_digit = false;
    for (const auto character : encoded) {
        if (character >= '0' && character <= '9') {
            if (value > 9999) return std::nullopt;
            value = value * 10 + (character - '0');
            has_digit = true;
            continue;
        }
        if (character != ';' || result.size == result.values.size()) return std::nullopt;
        result.values[result.size++] = has_digit ? value : 0;
        value = 0;
        has_digit = false;
    }
    if (result.size == result.values.size()) return std::nullopt;
    result.values[result.size++] = has_digit ? value : 0;
    return result;
}

TerminalModifiers decode_modifiers(i32 parameter) noexcept {
    if (parameter <= 1) return TerminalModifiers::NONE;
    // Xterm and Kitty encode modifiers as one plus a bit set containing
    // Shift, Alt, Control, and Super in that order.
    const auto bits = static_cast<u32>(parameter - 1);
    auto modifiers = TerminalModifiers::NONE;
    if ((bits & 1) != 0) modifiers = modifiers | TerminalModifiers::SHIFT;
    if ((bits & 2) != 0) modifiers = modifiers | TerminalModifiers::ALT;
    if ((bits & 4) != 0) modifiers = modifiers | TerminalModifiers::CONTROL;
    if ((bits & 8) != 0) modifiers = modifiers | TerminalModifiers::SUPER;
    return modifiers;
}

TerminalKey tilde_key(i32 parameter) noexcept {
    switch (parameter) {
        case 1:
        case 7: return TerminalKey::HOME;
        case 2: return TerminalKey::INSERT;
        case 3: return TerminalKey::DELETE_KEY;
        case 4:
        case 8: return TerminalKey::END;
        case 5: return TerminalKey::PAGE_UP;
        case 6: return TerminalKey::PAGE_DOWN;
        case 11: return TerminalKey::F1;
        case 12: return TerminalKey::F2;
        case 13: return TerminalKey::F3;
        case 14: return TerminalKey::F4;
        case 15: return TerminalKey::F5;
        case 17: return TerminalKey::F6;
        case 18: return TerminalKey::F7;
        case 19: return TerminalKey::F8;
        case 20: return TerminalKey::F9;
        case 21: return TerminalKey::F10;
        case 23: return TerminalKey::F11;
        case 24: return TerminalKey::F12;
        default: return TerminalKey::UNKNOWN;
    }
}

TerminalKey final_key(char final) noexcept {
    switch (final) {
        case 'A': return TerminalKey::ARROW_UP;
        case 'B': return TerminalKey::ARROW_DOWN;
        case 'C': return TerminalKey::ARROW_RIGHT;
        case 'D': return TerminalKey::ARROW_LEFT;
        case 'H': return TerminalKey::HOME;
        case 'F': return TerminalKey::END;
        case 'P': return TerminalKey::F1;
        case 'Q': return TerminalKey::F2;
        case 'R': return TerminalKey::F3;
        case 'S': return TerminalKey::F4;
        default: return TerminalKey::UNKNOWN;
    }
}

TerminalKey linux_console_function_key(char final) noexcept {
    if (final < 'A' || final > 'E') return TerminalKey::UNKNOWN;
    return static_cast<TerminalKey>(static_cast<u8>(TerminalKey::F1) + static_cast<u8>(final - 'A'));
}

std::optional<DecodedEscape> decode_escape(std::string_view sequence) {
    // The Linux virtual console uses CSI [ A through CSI [ E for F1-F5.
    if (sequence.starts_with("\x1b[[")) {
        if (sequence.size() < 4) return std::nullopt;
        return DecodedEscape{.key = linux_console_function_key(sequence[3]), .size = 4};
    }

    const bool csi = sequence.starts_with("\x1b[");
    const bool ss3 = sequence.starts_with("\x1bO");
    if (!csi && !ss3) return std::nullopt;
    usize final_offset = 2;
    while (final_offset < sequence.size() && !csi_final(static_cast<u8>(sequence[final_offset]))) ++final_offset;
    if (final_offset == sequence.size()) return std::nullopt;

    DecodedEscape result{.size = final_offset + 1};
    auto parameters = parse_csi_parameters(sequence.substr(2, final_offset - 2));
    if (!parameters) return result;

    const auto final = sequence[final_offset];
    if (csi && final == '~') {
        if (parameters->size == 0 || parameters->size > 2) return result;
        result.key = tilde_key(parameters->values[0]);
    } else {
        if (parameters->size > 2 || (parameters->size != 0 && parameters->values[0] != 1)) return result;
        result.key = final_key(final);
    }
    if (parameters->size == 2) result.modifiers = decode_modifiers(parameters->values[1]);
    return result;
}

bool incomplete_escape(std::string_view sequence) {
    if (sequence == "\x1b") return true;
    if (sequence.starts_with("\x1b[[") && sequence.size() < 4) return true;
    if (sequence.starts_with("\x1b[") || sequence.starts_with("\x1bO")) {
        return std::ranges::none_of(sequence.substr(2), [](char character) { return csi_final(static_cast<u8>(character)); });
    }
    return false;
}

std::string control_key_text(u8 byte) {
    if (byte == 0) {
        // POSIX cannot distinguish Ctrl+Space from Ctrl+@. Prefer the former,
        // which is the common interactive binding.
        return " ";
    }
    if (byte <= 0x1a) {
        return std::string(1, static_cast<char>('a' + byte - 1));
    }
    return std::string(1, static_cast<char>(byte + 0x40));
}

} // namespace

void TerminalInputDecoder::feed(std::string_view bytes, std::function_ref<void(TerminalEvent)> emit) {
    input.append(bytes);
    parse(false, emit);
}

void TerminalInputDecoder::flush_escape(std::function_ref<void(TerminalEvent)> emit) { parse(true, emit); }

bool TerminalInputDecoder::escape_pending() const noexcept {
    return !pasting && !input.empty() && input.front() == '\x1b' && incomplete_escape(input);
}

void TerminalInputDecoder::parse(bool flush_escape, std::function_ref<void(TerminalEvent)> emit) {
    while (!input.empty()) {
        if (pasting) {
            const auto end = input.find(k_paste_end);
            if (end == std::string::npos) {
                const usize keep = std::min(input.size(), k_paste_end.size() - 1);
                if (input.size() > keep) {
                    paste.append(input.data(), input.size() - keep);
                    input.erase(0, input.size() - keep);
                }
                return;
            }
            paste.append(input.data(), end);
            input.erase(0, end + k_paste_end.size());
            emit(TerminalEvent{.kind = TerminalEventKind::PASTE, .text = std::exchange(paste, {})});
            pasting = false;
            continue;
        }

        if (input.starts_with(k_paste_start)) {
            input.erase(0, k_paste_start.size());
            paste.clear();
            pasting = true;
            continue;
        }
        if (input.starts_with("\x1b[I")) {
            input.erase(0, 3);
            emit(TerminalEvent{.kind = TerminalEventKind::FOCUS, .focused = true});
            continue;
        }
        if (input.starts_with("\x1b[O")) {
            input.erase(0, 3);
            emit(TerminalEvent{.kind = TerminalEventKind::FOCUS, .focused = false});
            continue;
        }
        if (input.front() == '\x1b') {
            if (auto decoded = decode_escape(input)) {
                input.erase(0, decoded->size);
                emit(key_event(decoded->key, decoded->modifiers));
                continue;
            }
            if (!flush_escape && incomplete_escape(input)) {
                return;
            }
            input.erase(0, 1);
            emit(key_event(TerminalKey::ESCAPE));
            continue;
        }

        const auto byte = static_cast<u8>(input.front());
        if (byte == '\r') {
            input.erase(0, 1);
            emit(key_event(TerminalKey::ENTER));
            continue;
        }
        if (byte == '\n') {
            input.erase(0, 1);
            emit(key_event(TerminalKey::CHARACTER, TerminalModifiers::CONTROL, "j"));
            continue;
        }
        if (byte == 0x7f || byte == 0x08) {
            input.erase(0, 1);
            emit(key_event(TerminalKey::BACKSPACE));
            continue;
        }
        if (byte == '\t') {
            input.erase(0, 1);
            emit(key_event(TerminalKey::TAB));
            continue;
        }
        if (byte < 0x20) {
            input.erase(0, 1);
            emit(key_event(TerminalKey::CHARACTER, TerminalModifiers::CONTROL, control_key_text(byte)));
            continue;
        }

        usize end = 0;
        while (end < input.size()) {
            const auto current = static_cast<u8>(input[end]);
            if (current == 0x1b || current < 0x20 || current == 0x7f) {
                break;
            }
            ++end;
        }
        const auto complete = encoding::utf8::complete_prefix_len(std::string_view(input).substr(0, end));
        if (complete == 0) {
            return;
        }
        auto text = encoding::utf8::sanitize(std::string_view(input).substr(0, complete));
        input.erase(0, complete);
        emit(text_event(std::move(text)));
    }
}

} // namespace lighter::detail
