#include "terminal_input_decoder.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <lighter/encoding/utf8.h>

namespace lighter::detail {

namespace {

constexpr std::string_view k_paste_start = "\x1b[200~";
constexpr std::string_view k_paste_end = "\x1b[201~";

struct EscapeSequence {
    std::string_view encoded;
    TerminalKey key;
};

constexpr EscapeSequence k_key_sequences[] = {
    {"\x1b[A", TerminalKey::ARROW_UP},   {"\x1b[B", TerminalKey::ARROW_DOWN},  {"\x1b[C", TerminalKey::ARROW_RIGHT},
    {"\x1b[D", TerminalKey::ARROW_LEFT}, {"\x1b[H", TerminalKey::HOME},        {"\x1b[F", TerminalKey::END},
    {"\x1b[2~", TerminalKey::INSERT},    {"\x1b[3~", TerminalKey::DELETE_KEY}, {"\x1b[5~", TerminalKey::PAGE_UP},
    {"\x1b[6~", TerminalKey::PAGE_DOWN}, {"\x1bOP", TerminalKey::F1},          {"\x1bOQ", TerminalKey::F2},
    {"\x1bOR", TerminalKey::F3},         {"\x1bOS", TerminalKey::F4},          {"\x1b[15~", TerminalKey::F5},
    {"\x1b[17~", TerminalKey::F6},       {"\x1b[18~", TerminalKey::F7},        {"\x1b[19~", TerminalKey::F8},
    {"\x1b[20~", TerminalKey::F9},       {"\x1b[21~", TerminalKey::F10},       {"\x1b[23~", TerminalKey::F11},
    {"\x1b[24~", TerminalKey::F12},
};

struct DecodedEscape {
    TerminalKey key;
    usize size;
};

TerminalEvent key_event(TerminalKey key, TerminalModifiers modifiers = TerminalModifiers::NONE, std::string text = {}) {
    return TerminalEvent{.kind = TerminalEventKind::KEY, .key = key, .modifiers = modifiers, .text = std::move(text)};
}

TerminalEvent text_event(std::string text) { return TerminalEvent{.kind = TerminalEventKind::TEXT, .text = std::move(text)}; }

std::optional<DecodedEscape> decode_escape(std::string_view sequence) {
    for (const auto &[encoded, key] : k_key_sequences) {
        if (sequence.starts_with(encoded)) {
            return DecodedEscape{.key = key, .size = encoded.size()};
        }
    }
    return std::nullopt;
}

bool incomplete_escape(std::string_view sequence) {
    if (k_paste_start.starts_with(sequence) || k_paste_end.starts_with(sequence) || std::string_view("\x1b[I").starts_with(sequence) ||
        std::string_view("\x1b[O").starts_with(sequence)) {
        return true;
    }
    return std::ranges::any_of(k_key_sequences, [sequence](const auto &candidate) { return candidate.encoded.starts_with(sequence); });
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

void TerminalInputDecoder::feed(std::string_view bytes, FunctionRef<void(TerminalEvent)> emit) {
    input.append(bytes);
    parse(false, emit);
}

void TerminalInputDecoder::flush_escape(FunctionRef<void(TerminalEvent)> emit) { parse(true, emit); }

bool TerminalInputDecoder::escape_pending() const noexcept {
    return !pasting && !input.empty() && input.front() == '\x1b' && incomplete_escape(input);
}

void TerminalInputDecoder::parse(bool flush_escape, FunctionRef<void(TerminalEvent)> emit) {
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
                emit(key_event(decoded->key));
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
        if (byte == '\r' || byte == '\n') {
            input.erase(0, 1);
            emit(key_event(TerminalKey::ENTER));
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
