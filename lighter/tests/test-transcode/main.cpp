#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/encoding/transcode.h>
#include <lighter/encoding/utf8.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;
using namespace std::string_view_literals;
using encoding::Transcoder;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string convert(std::string_view bytes, std::string_view from, std::string_view to = "UTF-8") {
    auto transcoder = Transcoder::open(from, to);
    require(static_cast<bool>(transcoder), "open failed for " + std::string(from) + " -> " + std::string(to));
    std::string out;
    transcoder->feed(bytes, out);
    transcoder->finish(out);
    return out;
}

std::string convert_chunked(std::string_view bytes, std::string_view from, usize chunk) {
    auto transcoder = Transcoder::open(from);
    require(static_cast<bool>(transcoder), "open failed for " + std::string(from));
    std::string out;
    for (usize off = 0; off < bytes.size(); off += chunk) {
        transcoder->feed(bytes.substr(off, chunk), out);
    }
    transcoder->finish(out);
    return out;
}

/// The core robustness property, same as the utf8::Sanitizer tests: any
/// chunking must produce byte-identical output to the whole-buffer pass.
void check_any_split(std::string_view bytes, std::string_view from, std::string_view label) {
    const auto expected = convert(bytes, from);
    for (usize chunk = 1; chunk <= bytes.size(); ++chunk) {
        const auto got = convert_chunked(bytes, from, chunk);
        require(got == expected, std::string(label) + ": chunk size " + std::to_string(chunk) + " diverges");
    }
}

// -- label resolution --------------------------------------------------------

void test_resolve_label() {
    using encoding::resolve_label;
    require(resolve_label("GBK") == "GBK", "case-insensitive gbk");
    require(resolve_label("gb2312") == "GBK", "gb2312 means GBK on the web");
    require(resolve_label(" Shift_JIS ") == "CP932", "whitespace-trimmed shift_jis means CP932");
    require(resolve_label("latin1") == "CP1252", "latin1 means windows-1252 on the web");
    require(resolve_label("us-ascii") == "CP1252", "us-ascii means windows-1252 on the web");
    require(resolve_label("EUC-KR") == "CP949", "euc-kr means UHC");
    require(resolve_label("big5") == "CP950", "big5 means CP950");
    require(resolve_label("utf-16") == "UTF-16LE", "BOM-less utf-16 decodes as LE");
    require(resolve_label("utf8") == "UTF-8", "utf8 alias");
    require(!resolve_label("definitely-not-a-charset"), "unknown label");
    require(!resolve_label(""), "empty label");
    require(!resolve_label("   "), "blank label");
}

void test_open_unknown_encoding() {
    auto transcoder = Transcoder::open("definitely-not-a-charset");
    require(!transcoder, "open must fail for unsupported encodings");
}

// -- decoding common legacy encodings (vectors verified against Python) ------

void test_decode_common_encodings() {
    require(convert("\xd6\xd0\xce\xc4\xb2\xe2\xca\xd4", "gbk") == "中文测试", "gbk decode");
    require(convert("\xd6\xd0\xce\xc4", "gb18030") == "中文", "gb18030 decode");
    require(convert("\x93\xfa\x96\x7b\x8c\xea", "shift_jis") == "日本語", "shift_jis decode");
    require(convert("\xc6\xfc\xcb\xdc\xb8\xec", "euc-jp") == "日本語", "euc-jp decode");
    require(convert("\x1b\x24\x42\x46\x7c\x4b\x5c\x38\x6c\x1b\x28\x42", "iso-2022-jp") == "日本語", "iso-2022-jp decode");
    require(convert("\xb4\xfa\xb8\xd5", "big5") == "測試", "big5 decode");
    require(convert("\xc7\xd1\xb1\xdb", "euc-kr") == "한글", "euc-kr decode");
    require(convert("caf\xe9 \x80 1", "windows-1252") == "café € 1", "cp1252 decode");
    // sv literal: embedded NUL bytes must survive (plain literals truncate)
    require(convert("\x2d\x4e\x87\x65\x41\x00"sv, "utf-16le") == "中文A", "utf-16le decode");
}

// -- streaming: torn sequences across any chunk boundary ---------------------

void test_any_split() {
    check_any_split("ascii \xd6\xd0\xce\xc4 mixed \xb2\xe2\xca\xd4 tail", "gbk", "gbk");
    check_any_split("go \x93\xfa\x96\x7b\x8c\xea end", "shift_jis", "shift_jis");
    // stateful input: escape sequences may straddle chunks
    check_any_split("\x1b\x24\x42\x46\x7c\x4b\x5c\x38\x6c\x1b\x28\x42 abc", "iso-2022-jp", "iso-2022-jp");
    // fixed-width input: odd-length chunks tear every other code unit
    check_any_split("\x2d\x4e\x87\x65\x41\x00\x42\x00"sv, "utf-16le", "utf-16le");
    // invalid bytes interleaved with valid sequences
    check_any_split("ok\xff\xd6\xd0\xff go", "gbk", "gbk with invalid bytes");
}

void test_streaming_carry() {
    auto transcoder = Transcoder::open("gbk");
    require(static_cast<bool>(transcoder), "open gbk");
    std::string out;
    transcoder->feed("ab\xd6", out); // 0xd6 is a GBK lead byte
    require(out == "ab", "torn lead byte must not be emitted");
    require(transcoder->pending() == 1, "lead byte carried");
    transcoder->feed("\xd0", out);
    require(out == "ab中", "carried lead must complete");
    require(transcoder->pending() == 0, "carry consumed");
    transcoder->finish(out);
    require(out == "ab中", "clean finish adds nothing");

    // reuse after finish: state must be fully reset
    out.clear();
    transcoder->feed("\xce\xc4", out);
    transcoder->finish(out);
    require(out == "文", "transcoder must be reusable after finish");
}

// -- replacement policy ------------------------------------------------------

void test_invalid_input_replacement() {
    require(convert("ok\xff go", "gbk") == "ok\xEF\xBF\xBD go", "invalid byte becomes U+FFFD");

    // truncated trailing sequence: finish() flushes one replacement
    auto transcoder = Transcoder::open("gbk");
    require(static_cast<bool>(transcoder), "open gbk");
    std::string out;
    transcoder->feed("ab\xd6", out);
    transcoder->finish(out);
    require(out == "ab\xEF\xBF\xBD", "abandoned lead becomes one U+FFFD");
}

void test_encode_direction() {
    require(convert("中文", "utf-8", "gbk") == "\xd6\xd0\xce\xc4", "utf-8 -> gbk");

    // valid but unrepresentable input: replaced (GBK cannot encode U+FFFD, so '?')
    require(convert("a\xF0\x9F\x98\x80z", "utf-8", "gbk") == "a?z", "unconvertible emoji collapses to one replacement");

    // ill-formed utf-8 input: one replacement per maximal subpart
    require(convert("a\xE4\xB8z", "utf-8", "gbk") == "a?z", "truncated utf-8 sequence yields one replacement");

    // stateful target: finish() must emit the closing shift sequence
    require(convert("日本語", "utf-8", "iso-2022-jp") == "\x1b\x24\x42\x46\x7c\x4b\x5c\x38\x6c\x1b\x28\x42",
            "utf-8 -> iso-2022-jp with closing shift");
}

void test_utf8_passthrough() {
    // UTF-8 -> UTF-8 doubles as a sanitizer; policy matches utf8::sanitize
    const std::string_view mixed = "ok 中文 \xF0\x9F\x98\x80 fine";
    require(convert(mixed, "utf-8") == mixed, "well-formed utf-8 passes through");
    require(convert("a\xE4\xB8z", "utf-8") == encoding::utf8::sanitize("a\xE4\xB8z"), "utf-8 replacement matches sanitize()");
}

} // namespace

int main() {
    test_resolve_label();
    test_open_unknown_encoding();
    test_decode_common_encodings();
    test_any_split();
    test_streaming_carry();
    test_invalid_input_replacement();
    test_encode_direction();
    test_utf8_passthrough();
    return 0;
}
