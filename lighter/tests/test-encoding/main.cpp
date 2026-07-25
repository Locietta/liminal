#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/encoding/utf8.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;
using namespace std::string_view_literals;
namespace utf8 = encoding::utf8;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string sanitize_streamed(std::string_view stream, usize chunk) {
    utf8::Sanitizer sanitizer;
    std::string out;
    for (usize off = 0; off < stream.size(); off += chunk) {
        sanitizer.feed(stream.substr(off, chunk), out);
    }
    sanitizer.finish(out);
    return out;
}

/// The core robustness property: for any chunking of the stream, the
/// incremental sanitizer must produce byte-identical output to sanitize()
/// over the whole buffer.
void check_any_split(std::string_view stream, std::string_view label) {
    const auto expected = utf8::sanitize(stream);
    for (usize chunk = 1; chunk <= stream.size(); ++chunk) {
        const auto got = sanitize_streamed(stream, chunk);
        require(got == expected, std::string(label) + ": chunk size " + std::to_string(chunk) + " diverges from sanitize()");
    }
}

// -- decode_one --------------------------------------------------------------

void test_decode_well_formed() {
    struct Case {
        std::string_view bytes;
        char32_t codepoint;
        u8 size;
    };
    const Case cases[] = {
        {"A", U'A', 1},
        {"\x7F", 0x7F, 1},
        {"\xC2\x80", 0x80, 2},             // lowest 2-byte
        {"\xDF\xBF", 0x7FF, 2},            // highest 2-byte
        {"\xE0\xA0\x80", 0x800, 3},        // lowest 3-byte
        {"\xE4\xB8\xAD", U'中', 3},        // CJK, the common case for us
        {"\xED\x9F\xBF", 0xD7FF, 3},       // just below surrogates
        {"\xEE\x80\x80", 0xE000, 3},       // just above surrogates
        {"\xEF\xBF\xBD", 0xFFFD, 3},       // U+FFFD itself
        {"\xF0\x90\x80\x80", 0x10000, 4},  // lowest 4-byte
        {"\xF0\x9F\x98\x80", 0x1F600, 4},  // emoji
        {"\xF4\x8F\xBF\xBF", 0x10FFFF, 4}, // highest scalar value
    };
    for (const auto &c : cases) {
        const auto decoded = utf8::decode_one(c.bytes);
        require(decoded.status == utf8::DecodeStatus::OK, "decode: expected OK");
        require(decoded.codepoint == c.codepoint && decoded.size == c.size, "decode: codepoint/size mismatch");
    }
}

void test_decode_ill_formed() {
    struct Case {
        std::string_view bytes;
        u8 subpart; // expected maximal subpart length
    };
    const Case cases[] = {
        {"\x80", 1},             // stray continuation
        {"\xBF", 1},             // stray continuation
        {"\xC0\xAF", 1},         // overlong 2-byte lead (C0 never valid)
        {"\xC1\xBF", 1},         // overlong 2-byte lead (C1 never valid)
        {"\xF5\x80", 1},         // lead beyond U+10FFFF
        {"\xFF", 1},             // not a lead byte at all
        {"\xE0\x80\xAF", 1},     // overlong 3-byte: E0 requires A0..BF
        {"\xED\xA0\x80", 1},     // surrogate U+D800
        {"\xF0\x80\x80", 1},     // overlong 4-byte: F0 requires 90..BF
        {"\xF4\x90\x80", 1},     // above U+10FFFF: F4 allows 80..8F
        {"\xC2\x41", 1},         // 2-byte lead, ASCII follows
        {"\xE4\xB8\x41", 2},     // valid 2-byte prefix, then ASCII
        {"\xF0\x9F\x98\x41", 3}, // valid 3-byte prefix, then ASCII
        {"\xE4\xB8\xC2\x80", 2}, // valid prefix, then a new lead
    };
    for (const auto &c : cases) {
        const auto decoded = utf8::decode_one(c.bytes);
        require(decoded.status == utf8::DecodeStatus::INVALID, "decode: expected INVALID");
        require(decoded.size == c.subpart, "decode: maximal subpart length mismatch");
    }
}

void test_decode_incomplete() {
    const std::string_view cases[] = {
        "\xC2",         // 2-byte lead alone
        "\xE4",         // 3-byte lead alone
        "\xE4\xB8",     // 3-byte lead + 1 continuation
        "\xF0\x9F",     // 4-byte lead + 1 continuation
        "\xF0\x9F\x98", // 4-byte lead + 2 continuations
    };
    for (const auto &bytes : cases) {
        const auto decoded = utf8::decode_one(bytes);
        require(decoded.status == utf8::DecodeStatus::INCOMPLETE, "decode: expected INCOMPLETE");
        require(decoded.size == bytes.size(), "decode: INCOMPLETE must consume all remaining bytes");
    }
}

void test_encode_round_trip() {
    const char32_t cases[] = {U'A', 0x7F, 0x80, 0x7FF, 0x800, U'中', 0xD7FF, 0xE000, 0xFFFD, 0x10000, 0x1F600, 0x10FFFF};
    for (const auto codepoint : cases) {
        const auto encoded = utf8::encode_one(codepoint);
        require(encoded.size > 0, "encode: valid scalar must encode");
        const auto decoded = utf8::decode_one(encoded.view());
        require(decoded.status == utf8::DecodeStatus::OK && decoded.codepoint == codepoint && decoded.size == encoded.size,
                "encode/decode round trip mismatch");
    }
    require(utf8::encode_one(0xD800).size == 0, "encode: surrogate must be rejected");
    require(utf8::encode_one(0x110000).size == 0, "encode: beyond U+10FFFF must be rejected");
}

// -- validation --------------------------------------------------------------

void test_find_invalid() {
    require(utf8::is_valid(""), "empty input is valid");
    require(utf8::is_valid("plain ascii"), "ascii is valid");
    require(utf8::is_valid("中文 emoji \xF0\x9F\x98\x80 done"), "well-formed multi-byte is valid");
    require(utf8::find_invalid("ok\x80rest") == 2, "stray continuation position");
    require(utf8::find_invalid("ab\xE4\xB8") == 2, "truncated tail counts as invalid for whole-buffer checks");
    require(utf8::find_invalid("\xED\xA0\x80") == 0, "surrogates are invalid");
}

void test_complete_prefix_len() {
    require(utf8::complete_prefix_len("") == 0, "empty");
    require(utf8::complete_prefix_len("ascii") == 5, "ascii is always complete");
    require(utf8::complete_prefix_len("ab\xE4\xB8\xAD") == 5, "complete multi-byte tail");
    require(utf8::complete_prefix_len("ab\xC2") == 2, "2-byte lead pending");
    require(utf8::complete_prefix_len("ab\xE4\xB8") == 2, "3-byte prefix pending");
    require(utf8::complete_prefix_len("ab\xF0\x9F\x98") == 2, "4-byte prefix pending");
    require(utf8::complete_prefix_len("\xF0\x9F\x98") == 0, "whole input pending");
    // ill-formed tails are NOT pending: no future byte can rescue them
    require(utf8::complete_prefix_len("ab\xED\xA0") == 4, "surrogate prefix is complete (ill-formed)");
    require(utf8::complete_prefix_len("ab\x80\x80") == 4, "stray continuations are complete");
    require(utf8::complete_prefix_len("ab\xF5\x80") == 4, "invalid lead is complete");
    // a lead hidden behind 3+ continuation bytes cannot be pending
    require(utf8::complete_prefix_len("\xF0\x90\x80\x80") == 4, "complete 4-byte sequence");
}

// -- sanitize ----------------------------------------------------------------

void test_sanitize() {
    require(utf8::sanitize("clean 中文") == "clean 中文", "well-formed input passes through");

    // Unicode 3.9's worked example: maximal subpart replacement
    require(utf8::sanitize("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64") == "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                                                                                      "b\xEF\xBF\xBD"
                                                                                      "c\xEF\xBF\xBD\xEF\xBF\xBD"
                                                                                      "d",
            "Unicode maximal subpart example mismatch");

    require(utf8::sanitize("tail\xE4\xB8") == "tail\xEF\xBF\xBD", "truncated tail becomes one U+FFFD");
    // GBK-encoded "中文" fed as if UTF-8: every byte pair is ill-formed
    require(utf8::sanitize("\xD6\xD0\xCE\xC4").find('\xD6') == std::string::npos, "GBK bytes must not leak through");
}

// -- streaming sanitizer -----------------------------------------------------

void test_sanitizer_any_split() {
    check_any_split("plain ascii, no multi-byte at all", "ascii");
    check_any_split("中文与 emoji \xF0\x9F\x98\x80 混排 mixed with ascii", "well-formed CJK/emoji");
    check_any_split("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64", "Unicode maximal subpart example");
    check_any_split("\xD6\xD0\xCE\xC4 GBK bytes \xB2\xE2\xCA\xD4 in a UTF-8 stream", "GBK mojibake");
    check_any_split("truncated at end \xF0\x9F\x98", "truncated tail");
    check_any_split("\xC2\xC2\xC2 lead runs \xE0\xE0", "consecutive leads");
    check_any_split("\x80\x80\x80\x80", "continuation runs");
    check_any_split("a\xED\xA0\x80z surrogate \xED\xBF\xBFy", "surrogates");
    check_any_split("\xF4\x8F\xBF\xBF\xF4\x90\x80\x80", "boundary of Unicode range");
}

void test_sanitizer_streaming_behavior() {
    // whole sequences flush immediately; a partial tail is held back
    utf8::Sanitizer sanitizer;
    std::string out;
    sanitizer.feed("abc\xE4\xB8", out);
    require(out == "abc", "partial sequence must not be emitted early");
    require(sanitizer.pending() == 2, "two bytes should be carried");
    sanitizer.feed("\xAD", out);
    require(out == "abc中", "carried prefix must complete");
    require(sanitizer.pending() == 0, "carry consumed");
    sanitizer.finish(out);
    require(out == "abc中", "finish with no carry adds nothing");

    // finish() flushes an abandoned prefix as one replacement
    utf8::Sanitizer truncated;
    out.clear();
    truncated.feed("x\xF0\x9F", out);
    truncated.finish(out);
    require(out == "x\xEF\xBF\xBD", "abandoned prefix becomes one U+FFFD");

    // a carried prefix invalidated by the next chunk
    utf8::Sanitizer invalidated;
    out.clear();
    invalidated.feed("y\xE4", out);
    invalidated.feed("Z", out); // E4 needs a continuation; Z is not one
    invalidated.finish(out);
    require(out == "y\xEF\xBF\xBDZ", "invalidated carry must replace and keep the new byte");
}

} // namespace

int main() {
    test_decode_well_formed();
    test_decode_ill_formed();
    test_decode_incomplete();
    test_encode_round_trip();
    test_find_invalid();
    test_complete_prefix_len();
    test_sanitize();
    test_sanitizer_any_split();
    test_sanitizer_streaming_behavior();
    return 0;
}
