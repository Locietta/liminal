#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/async/detail/terminal_input_decoder.h>
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
        {"\x1b[A", TerminalKey::ARROW_UP},
        {"\x1b[3~", TerminalKey::DELETE_KEY},
        {"\x1bOP", TerminalKey::F1},
        {"\x1b[24~", TerminalKey::F12},
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
    for (const u8 byte : {u8{0}, u8{1}, u8{26}, u8{28}, u8{29}, u8{30}, u8{31}}) {
        input.push_back(static_cast<char>(byte));
    }
    input.push_back('x');
    feed(decoder, input, events);

    constexpr std::string_view expected[] = {" ", "a", "z", "\\", "]", "^", "_"};
    require(events.size() == std::size(expected) + 1, "control bytes must not block later input");
    for (usize i = 0; i < std::size(expected); ++i) {
        require(events[i].kind == TerminalEventKind::KEY && events[i].key == TerminalKey::CHARACTER &&
                    events[i].modifiers == TerminalModifiers::CONTROL && events[i].text == expected[i],
                "control bytes must decode to their conventional key identity");
    }
    require(events.back().kind == TerminalEventKind::TEXT && events.back().text == "x", "input following NUL must still be emitted");
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

i32 run_all() {
    check_terminal_basics();
    check_text_and_utf8_chunking();
    check_key_sequences();
    check_escape_timeout();
    check_control_bytes();
    check_paste_chunking();
    check_focus_sequences();
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
