#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <lighter/async/async.h>
#include <lighter/async/detail/terminal_input_decoder.h>
#include <lighter/encoding/utf8.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void feed(detail::TerminalInputDecoder &decoder, std::string_view bytes, std::vector<TerminalEvent> &events) {
    auto emit = [&events](TerminalEvent event) { events.push_back(std::move(event)); };
    decoder.feed(bytes, emit);
}

void flush_escape(detail::TerminalInputDecoder &decoder, std::vector<TerminalEvent> &events) {
    auto emit = [&events](TerminalEvent event) { events.push_back(std::move(event)); };
    decoder.flush_escape(emit);
}

void check_terminal_basics() {
    require(!TerminalSession::attached(-1), "an invalid descriptor cannot be a terminal");

    EventLoop loop;
    auto invalid = TerminalSession::open(-1, -1, TerminalSession::Options(), loop);
    require(!invalid.has_value(), "opening invalid terminal descriptors must fail");

    const auto modifiers = TerminalModifiers::SHIFT | TerminalModifiers::CONTROL;
    require(has_modifier(modifiers, TerminalModifiers::SHIFT), "SHIFT modifier must survive composition");
    require(has_modifier(modifiers, TerminalModifiers::CONTROL), "CONTROL modifier must survive composition");
    require(!has_modifier(modifiers, TerminalModifiers::ALT), "unset modifiers must remain clear");

    TerminalEvent event{.kind = TerminalEventKind::RESIZE, .size = {.columns = 120, .rows = 40}};
    require(event.size == TerminalSize{120, 40}, "terminal size event must retain dimensions");
}

void check_text_and_utf8_chunking() {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;

    feed(decoder, std::string_view("a\xc3", 2), events);
    require(events.size() == 1 && events[0].kind == TerminalEventKind::TEXT && events[0].text == "a",
            "complete text before a partial UTF-8 sequence must be emitted");

    feed(decoder, "\xa9z", events);
    require(events.size() == 2 && events[1].kind == TerminalEventKind::TEXT && events[1].text == "\xc3\xa9z",
            "a split UTF-8 sequence must be carried and completed");
}

void check_key_sequences() {
    struct KeyCase {
        std::string_view encoded;
        TerminalKey key;
    };
    constexpr KeyCase cases[] = {
        {"\x1b[A", TerminalKey::ARROW_UP}, {"\x1b[3~", TerminalKey::DELETE_KEY}, {"\x1bOP", TerminalKey::F1},
        {"\x1b[[A", TerminalKey::F1},      {"\x1b[24~", TerminalKey::F12},
    };

    for (const auto &[encoded, expected] : cases) {
        detail::TerminalInputDecoder decoder;
        std::vector<TerminalEvent> events;
        for (const char byte : encoded) {
            feed(decoder, std::string_view(&byte, 1), events);
        }
        require(events.size() == 1 && events[0].kind == TerminalEventKind::KEY && events[0].key == expected,
                "a key sequence split at every byte must decode exactly once");
        require(!decoder.escape_pending(), "a complete key sequence must not retain escape state");
    }
}

void check_modified_and_unknown_escape_sequences() {
    struct ModifiedKeyCase {
        std::string_view encoded;
        TerminalKey key;
        TerminalModifiers modifiers;
    };
    constexpr ModifiedKeyCase cases[] = {
        {"\x1b[1;5D", TerminalKey::ARROW_LEFT, TerminalModifiers::CONTROL},
        {"\x1b[6;4~", TerminalKey::PAGE_DOWN, TerminalModifiers::SHIFT | TerminalModifiers::ALT},
        {"\x1b[1;9C", TerminalKey::ARROW_RIGHT, TerminalModifiers::SUPER},
    };

    for (const auto &[encoded, key, modifiers] : cases) {
        detail::TerminalInputDecoder decoder;
        std::vector<TerminalEvent> events;
        for (const char byte : encoded) {
            feed(decoder, std::string_view(&byte, 1), events);
        }
        require(events.size() == 1 && events[0].kind == TerminalEventKind::KEY && events[0].key == key && events[0].modifiers == modifiers,
                "a modified CSI key split at every byte must retain its key and modifiers");
    }

    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;
    for (const auto encoded : {std::string_view("\x1b[42;7z"), std::string_view("\x1bOZ")}) {
        for (const char byte : encoded) {
            feed(decoder, std::string_view(&byte, 1), events);
        }
        require(events.size() == 1 && events[0].kind == TerminalEventKind::KEY && events[0].key == TerminalKey::UNKNOWN,
                "an unsupported complete terminal key sequence must be consumed as one unknown key");
        events.clear();
    }
    feed(decoder, "x", events);
    require(events.size() == 1 && events[0].kind == TerminalEventKind::TEXT && events[0].text == "x",
            "text following unsupported terminal key sequences must remain intact");
}

void check_escape_timeout() {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;

    feed(decoder, "\x1b[", events);
    require(events.empty() && decoder.escape_pending(), "an incomplete escape sequence must wait for its timeout");
    flush_escape(decoder, events);
    require(events.size() == 2 && events[0].kind == TerminalEventKind::KEY && events[0].key == TerminalKey::ESCAPE &&
                events[1].kind == TerminalEventKind::TEXT && events[1].text == "[",
            "an expired escape prefix must preserve every input byte");
}

void check_control_bytes() {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;
    std::string input;
    for (const u8 byte : {u8{0}, u8{1}, u8{7}, u8{26}, u8{28}, u8{29}, u8{30}, u8{31}}) {
        input.push_back(static_cast<char>(byte));
    }
    input.push_back('x');
    feed(decoder, input, events);

    constexpr std::string_view expected[] = {" ", "a", "g", "z", "\\", "]", "^", "_"};
    require(events.size() == std::size(expected) + 1, "control bytes must not block later input");
    for (usize i = 0; i < std::size(expected); ++i) {
        require(events[i].kind == TerminalEventKind::KEY && events[i].key == TerminalKey::CHARACTER &&
                    events[i].modifiers == TerminalModifiers::CONTROL && events[i].text == expected[i],
                "control bytes must decode to their conventional key identity");
    }
    require(events.back().kind == TerminalEventKind::TEXT && events.back().text == "x", "input following NUL must still be emitted");

    events.clear();
    feed(decoder, "\r\n", events);
    require(events.size() == 2 && events[0].key == TerminalKey::ENTER && events[0].modifiers == TerminalModifiers::NONE &&
                events[1].key == TerminalKey::CHARACTER && events[1].modifiers == TerminalModifiers::CONTROL && events[1].text == "j",
            "carriage return must submit while line feed remains the portable Ctrl+J newline binding");
}

void check_paste_chunking() {
    constexpr std::string_view start = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";

    for (usize start_split = 1; start_split < start.size(); ++start_split) {
        for (usize end_split = 1; end_split < end.size(); ++end_split) {
            detail::TerminalInputDecoder decoder;
            std::vector<TerminalEvent> events;
            feed(decoder, start.substr(0, start_split), events);
            feed(decoder, start.substr(start_split), events);
            feed(decoder, "one\ntwo", events);
            feed(decoder, end.substr(0, end_split), events);
            require(events.empty(), "paste content must wait for a complete end marker");
            feed(decoder, end.substr(end_split), events);
            require(events.size() == 1 && events[0].kind == TerminalEventKind::PASTE && events[0].text == "one\ntwo",
                    "paste markers split at arbitrary boundaries must preserve content");
        }
    }
}

void check_focus_sequences() {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;
    feed(decoder, "\x1b[I\x1b[O", events);
    require(events.size() == 2 && events[0].kind == TerminalEventKind::FOCUS && events[0].focused &&
                events[1].kind == TerminalEventKind::FOCUS && !events[1].focused,
            "focus sequences must retain their state");
}

void check_mouse_sequences() {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;

    constexpr std::string_view wheel_up = "\x1b[<92;12;7M";
    for (const char byte : wheel_up) feed(decoder, std::string_view(&byte, 1), events);
    require(events.size() == 1 && events[0].kind == TerminalEventKind::MOUSE && events[0].x == 11 && events[0].y == 6 &&
                events[0].wheel_delta == 120 &&
                events[0].modifiers == (TerminalModifiers::SHIFT | TerminalModifiers::ALT | TerminalModifiers::CONTROL),
            "an SGR wheel event split at every byte must normalize coordinates, direction, and modifiers");

    feed(decoder, "\x1b[<65;4;3M\x1b[<0;2;5M\x1b[<0;2;5m", events);
    require(events.size() == 4 && events[1].kind == TerminalEventKind::MOUSE && events[1].wheel_delta == -120,
            "an SGR wheel-down event must use the native Windows wheel direction convention");
    require(events[2].mouse_buttons == 1 && events[2].pressed && events[3].mouse_buttons == 0 && !events[3].pressed,
            "SGR button press and release events must retain normalized button state");
}

bool same_event(const TerminalEvent &lhs, const TerminalEvent &rhs) {
    return lhs.kind == rhs.kind && lhs.key == rhs.key && lhs.modifiers == rhs.modifiers && lhs.text == rhs.text && lhs.size == rhs.size &&
           lhs.repeat == rhs.repeat && lhs.x == rhs.x && lhs.y == rhs.y && lhs.mouse_buttons == rhs.mouse_buttons &&
           lhs.wheel_delta == rhs.wheel_delta && lhs.pressed == rhs.pressed && lhs.focused == rhs.focused;
}

std::vector<TerminalEvent> decode_chunks(std::string_view bytes, usize chunk_size) {
    detail::TerminalInputDecoder decoder;
    std::vector<TerminalEvent> events;
    for (usize offset = 0; offset < bytes.size(); offset += chunk_size) {
        feed(decoder, bytes.substr(offset, std::min(chunk_size, bytes.size() - offset)), events);
    }
    flush_escape(decoder, events);
    std::vector<TerminalEvent> normalized;
    for (auto &event : events) {
        if (event.kind == TerminalEventKind::TEXT && !normalized.empty() && normalized.back().kind == TerminalEventKind::TEXT) {
            normalized.back().text += event.text;
        } else {
            normalized.push_back(std::move(event));
        }
    }
    return normalized;
}

void check_decoder_chunk_fuzz() {
    u32 state = 0x9e3779b9;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    for (usize sample = 0; sample < 500; ++sample) {
        std::string input;
        const auto size = static_cast<usize>(next() % 257);
        input.reserve(size);
        for (usize index = 0; index < size; ++index) input.push_back(static_cast<char>(next() & 0xff));

        const auto whole = decode_chunks(input, std::max<usize>(1, input.size()));
        const auto chunked = decode_chunks(input, 1 + next() % 11);
        require(whole.size() == chunked.size(), "terminal decoding must be invariant under arbitrary byte chunking");
        for (usize index = 0; index < whole.size(); ++index) {
            require(same_event(whole[index], chunked[index]), "chunked terminal decoding changed an emitted event");
            require(encoding::utf8::is_valid(whole[index].text), "terminal decoding must emit only valid UTF-8 text");
        }
    }
}

#ifdef _WIN32
void check_windows_console_restoration() {
    bool allocated = false;
    auto input =
        CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        require(AllocConsole(), "test process could not allocate a Windows console");
        allocated = true;
        input =
            CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto output =
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE, "Windows console handles could not be opened");

    DWORD original_input_mode = 0;
    DWORD original_output_mode = 0;
    require(GetConsoleMode(input, &original_input_mode) && GetConsoleMode(output, &original_output_mode),
            "Windows console modes could not be captured");
    const auto original_input_codepage = GetConsoleCP();
    const auto original_output_codepage = GetConsoleOutputCP();

    const auto input_fd = _open_osfhandle(reinterpret_cast<intptr_t>(input), _O_RDONLY);
    const auto output_fd = _open_osfhandle(reinterpret_cast<intptr_t>(output), _O_WRONLY);
    require(input_fd >= 0 && output_fd >= 0, "Windows console handles could not be exposed as file descriptors");
    {
        EventLoop loop;
        auto opened = TerminalSession::open(input_fd, output_fd, TerminalSession::Options(), loop);
        require(opened.has_value(), "Windows terminal session could not be opened");
        auto session = *std::move(opened);
        require(session.active(), "Windows terminal session must become active");
        require(!session.suspend(), "Windows terminal session could not suspend");
        DWORD suspended_input_mode = 0;
        DWORD suspended_output_mode = 0;
        require(GetConsoleMode(input, &suspended_input_mode) && GetConsoleMode(output, &suspended_output_mode),
                "suspended Windows console modes could not be read");
        require(suspended_input_mode == original_input_mode && suspended_output_mode == original_output_mode,
                "suspension must restore exact Windows console modes");
        require(GetConsoleCP() == original_input_codepage && GetConsoleOutputCP() == original_output_codepage,
                "suspension must restore exact Windows code pages");
        require(!session.resume() && session.active(), "Windows terminal session could not resume");
    }

    DWORD restored_input_mode = 0;
    DWORD restored_output_mode = 0;
    require(GetConsoleMode(input, &restored_input_mode) && GetConsoleMode(output, &restored_output_mode),
            "restored Windows console modes could not be read");
    require(restored_input_mode == original_input_mode && restored_output_mode == original_output_mode,
            "Windows terminal session must restore exact console modes");
    require(GetConsoleCP() == original_input_codepage && GetConsoleOutputCP() == original_output_codepage,
            "Windows terminal session must restore exact code pages");

    _close(input_fd);
    _close(output_fd);
    if (allocated) FreeConsole();
}
#endif

i32 run_all() {
    check_terminal_basics();
    check_text_and_utf8_chunking();
    check_key_sequences();
    check_modified_and_unknown_escape_sequences();
    check_escape_timeout();
    check_control_bytes();
    check_paste_chunking();
    check_focus_sequences();
    check_mouse_sequences();
    check_decoder_chunk_fuzz();
#ifdef _WIN32
    check_windows_console_restoration();
#endif
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
