#include "transcode.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <iconv.h>

#include <lighter/encoding/utf8.h>

namespace lighter::encoding {

namespace {

constexpr usize k_iconv_failed = static_cast<usize>(-1);

/// iconv()'s input parameter is `char **` or `const char **` depending on the
/// libiconv build; this adapter converts to whichever the prototype wants.
struct InArg {
    const char **p;

    operator char **() const noexcept { return const_cast<char **>(p); }

    operator const char **() const noexcept { return p; }
};

struct Label {
    std::string_view label;
    std::string_view canonical;
};

/// WHATWG Encoding Standard labels (pragmatic subset), mapped to iconv names.
/// Web-reality quirks are intentional: latin1/ascii mean windows-1252,
/// shift_jis means CP932, euc-kr means UHC (CP949), big5 means CP950.
constexpr Label k_labels[] = {
    // clang-format off
    {"utf-8", "UTF-8"}, {"utf8", "UTF-8"}, {"unicode-1-1-utf-8", "UTF-8"},
    {"gbk", "GBK"}, {"gb2312", "GBK"}, {"gb_2312", "GBK"}, {"gb_2312-80", "GBK"},
    {"chinese", "GBK"}, {"csgb2312", "GBK"}, {"csiso58gb231280", "GBK"},
    {"iso-ir-58", "GBK"}, {"x-gbk", "GBK"}, {"cp936", "GBK"}, {"ms936", "GBK"},
    {"gb18030", "GB18030"},
    {"big5", "CP950"}, {"cn-big5", "CP950"}, {"csbig5", "CP950"}, {"x-x-big5", "CP950"},
    {"cp950", "CP950"}, {"big5-hkscs", "BIG5-HKSCS"},
    {"shift_jis", "CP932"}, {"shift-jis", "CP932"}, {"sjis", "CP932"}, {"x-sjis", "CP932"},
    {"ms_kanji", "CP932"}, {"csshiftjis", "CP932"}, {"windows-31j", "CP932"},
    {"ms932", "CP932"}, {"cp932", "CP932"},
    {"euc-jp", "EUC-JP"}, {"x-euc-jp", "EUC-JP"}, {"cseucpkdfmtjapanese", "EUC-JP"},
    {"iso-2022-jp", "ISO-2022-JP"}, {"csiso2022jp", "ISO-2022-JP"},
    {"euc-kr", "CP949"}, {"windows-949", "CP949"}, {"cp949", "CP949"}, {"uhc", "CP949"},
    {"korean", "CP949"}, {"ks_c_5601-1987", "CP949"}, {"ks_c_5601-1989", "CP949"},
    {"ksc5601", "CP949"}, {"ksc_5601", "CP949"}, {"csksc56011987", "CP949"}, {"iso-ir-149", "CP949"},
    {"windows-1252", "CP1252"}, {"cp1252", "CP1252"}, {"x-cp1252", "CP1252"},
    {"iso-8859-1", "CP1252"}, {"iso8859-1", "CP1252"}, {"iso88591", "CP1252"},
    {"iso_8859-1", "CP1252"}, {"iso_8859-1:1987", "CP1252"}, {"latin1", "CP1252"},
    {"l1", "CP1252"}, {"csisolatin1", "CP1252"}, {"cp819", "CP1252"}, {"ibm819", "CP1252"},
    {"ascii", "CP1252"}, {"us-ascii", "CP1252"}, {"ansi_x3.4-1968", "CP1252"},
    {"windows-1250", "CP1250"}, {"cp1250", "CP1250"}, {"x-cp1250", "CP1250"},
    {"windows-1251", "CP1251"}, {"cp1251", "CP1251"}, {"x-cp1251", "CP1251"},
    {"windows-1253", "CP1253"}, {"cp1253", "CP1253"}, {"x-cp1253", "CP1253"},
    {"windows-1254", "CP1254"}, {"cp1254", "CP1254"}, {"x-cp1254", "CP1254"},
    {"windows-1255", "CP1255"}, {"cp1255", "CP1255"}, {"x-cp1255", "CP1255"},
    {"windows-1256", "CP1256"}, {"cp1256", "CP1256"}, {"x-cp1256", "CP1256"},
    {"windows-1257", "CP1257"}, {"cp1257", "CP1257"}, {"x-cp1257", "CP1257"},
    {"windows-1258", "CP1258"}, {"cp1258", "CP1258"}, {"x-cp1258", "CP1258"},
    {"iso-8859-2", "ISO-8859-2"}, {"iso8859-2", "ISO-8859-2"}, {"latin2", "ISO-8859-2"}, {"l2", "ISO-8859-2"},
    {"iso-8859-5", "ISO-8859-5"}, {"iso8859-5", "ISO-8859-5"}, {"cyrillic", "ISO-8859-5"},
    {"iso-8859-7", "ISO-8859-7"}, {"iso8859-7", "ISO-8859-7"}, {"greek", "ISO-8859-7"},
    {"iso-8859-15", "ISO-8859-15"}, {"iso8859-15", "ISO-8859-15"}, {"latin9", "ISO-8859-15"}, {"l9", "ISO-8859-15"},
    {"koi8-r", "KOI8-R"}, {"koi8", "KOI8-R"}, {"koi8_r", "KOI8-R"}, {"cskoi8r", "KOI8-R"},
    {"koi8-u", "KOI8-U"}, {"koi8-ru", "KOI8-U"},
    {"windows-874", "CP874"}, {"dos-874", "CP874"}, {"tis-620", "CP874"},
    {"iso-8859-11", "CP874"}, {"iso8859-11", "CP874"},
    // WHATWG decodes BOM-less utf-16 as little-endian
    {"utf-16", "UTF-16LE"}, {"utf-16le", "UTF-16LE"}, {"ucs-2", "UTF-16LE"},
    {"unicode", "UTF-16LE"}, {"unicodefeff", "UTF-16LE"}, {"csunicode", "UTF-16LE"},
    {"iso-10646-ucs-2", "UTF-16LE"},
    {"utf-16be", "UTF-16BE"}, {"unicodefffe", "UTF-16BE"},
    // clang-format on
};

std::optional<std::string_view> lookup_label(std::string_view lowered) noexcept {
    for (const auto &entry : k_labels) {
        if (entry.label == lowered) {
            return entry.canonical;
        }
    }
    return std::nullopt;
}

/// The bytes of the replacement character in the target encoding: U+FFFD
/// where representable, otherwise '?'. Computed once per Transcoder with a
/// scratch descriptor so any target encoding works (e.g. FD FF for UTF-16LE).
std::string replacement_for(const char *to_name) {
    iconv_t cd = iconv_open(to_name, "UTF-8");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return "?";
    }

    std::string result = "?";
    for (const std::string_view candidate : {std::string_view(utf8::k_replacement), std::string_view("?")}) {
        iconv(cd, nullptr, nullptr, nullptr, nullptr); // reset shift state
        const char *in = candidate.data();
        usize in_left = candidate.size();
        char buf[32];
        char *dst = buf;
        usize dst_left = sizeof(buf);
        if (iconv(cd, InArg{&in}, &in_left, &dst, &dst_left) == k_iconv_failed || in_left != 0) {
            continue;
        }
        iconv(cd, nullptr, nullptr, &dst, &dst_left); // shift back to be self-contained
        result.assign(buf, sizeof(buf) - dst_left);
        break;
    }

    iconv_close(cd);
    return result;
}

} // namespace

std::optional<std::string_view> resolve_label(std::string_view label) noexcept {
    constexpr std::string_view k_space = " \t\r\n\f";
    if (const auto begin = label.find_first_not_of(k_space); begin != std::string_view::npos) {
        label = label.substr(begin, label.find_last_not_of(k_space) - begin + 1);
    } else {
        return std::nullopt;
    }

    char lowered[24];
    if (label.size() > sizeof(lowered)) {
        return std::nullopt;
    }
    for (usize i = 0; i < label.size(); ++i) {
        const char c = label[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return lookup_label(std::string_view(lowered, label.size()));
}

Result<Transcoder> Transcoder::open(std::string_view from, std::string_view to) {
    const std::string from_name(resolve_label(from).value_or(from));
    const std::string to_name(resolve_label(to).value_or(to));

    iconv_t cd = iconv_open(to_name.c_str(), from_name.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return outcome_error(Error::k_invalid_argument);
    }

    Transcoder out;
    out.cd = cd;
    out.utf8_input = from_name == "UTF-8";
    out.replacement = replacement_for(to_name.c_str());
    return out;
}

Transcoder::Transcoder(Transcoder &&other) noexcept
    : cd(std::exchange(other.cd, nullptr)), carry(std::move(other.carry)), replacement(std::move(other.replacement)),
      utf8_input(other.utf8_input) {}

Transcoder &Transcoder::operator=(Transcoder &&other) noexcept {
    if (this != &other) {
        if (cd != nullptr) {
            iconv_close(static_cast<iconv_t>(cd));
        }
        cd = std::exchange(other.cd, nullptr);
        carry = std::move(other.carry);
        replacement = std::move(other.replacement);
        utf8_input = other.utf8_input;
    }
    return *this;
}

Transcoder::~Transcoder() {
    if (cd != nullptr) {
        iconv_close(static_cast<iconv_t>(cd));
    }
}

void Transcoder::run(const char *&in, usize &in_left, std::string &out) {
    char buf[4096];
    while (in_left > 0) {
        char *dst = buf;
        usize dst_left = sizeof(buf);
        const auto rc = iconv(static_cast<iconv_t>(cd), InArg{&in}, &in_left, &dst, &dst_left);
        const int err = rc == k_iconv_failed ? errno : 0; // before append can clobber errno
        out.append(buf, sizeof(buf) - dst_left);

        if (rc != k_iconv_failed || err == E2BIG) {
            continue; // done, or the scratch buffer filled: go again
        }
        if (err == EINVAL) {
            return; // incomplete sequence at end of input: caller carries it
        }

        // EILSEQ: invalid input, or valid input the target cannot represent.
        // Replace and continue, mirroring utf8::Sanitizer. For UTF-8 input,
        // skip the whole maximal subpart / character so one bad or
        // unconvertible character yields exactly one replacement; for legacy
        // input, one byte per replacement matches the usual decoder behavior.
        out.append(replacement);
        usize skip = 1;
        if (utf8_input) {
            skip = utf8::decode_one(std::string_view(in, in_left)).size;
        }
        skip = std::clamp<usize>(skip, 1, in_left);
        in += skip;
        in_left -= skip;
    }
}

void Transcoder::feed(std::string_view bytes, std::string &out) {
    if (bytes.empty()) {
        return;
    }

    if (!carry.empty()) {
        // rare (a sequence straddled the previous chunk boundary), so the
        // extra copy is fine; the leftover tail becomes the next carry
        carry.append(bytes);
        const char *in = carry.data();
        usize in_left = carry.size();
        run(in, in_left, out);
        carry.erase(0, carry.size() - in_left);
        return;
    }

    const char *in = bytes.data();
    usize in_left = bytes.size();
    run(in, in_left, out);
    carry.assign(in, in_left);
}

void Transcoder::finish(std::string &out) {
    if (!carry.empty()) {
        // a truncated trailing sequence can no longer complete
        out.append(replacement);
        carry.clear();
    }

    // emit the closing shift sequence of stateful targets (ISO-2022-JP) and
    // reset the conversion state so the Transcoder is reusable
    char buf[64];
    char *dst = buf;
    usize dst_left = sizeof(buf);
    iconv(static_cast<iconv_t>(cd), nullptr, nullptr, &dst, &dst_left);
    out.append(buf, sizeof(buf) - dst_left);
}

Result<std::string> to_utf8(std::string_view bytes, std::string_view from) {
    auto transcoder = Transcoder::open(from);
    if (!transcoder) {
        return outcome_error(std::move(transcoder).error());
    }

    std::string out;
    out.reserve(bytes.size() + bytes.size() / 2);
    transcoder->feed(bytes, out);
    transcoder->finish(out);
    return out;
}

} // namespace lighter::encoding
