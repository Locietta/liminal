#include "utf8.h"

#include <cassert>
#include <cstring>

namespace lighter::encoding::utf8 {

Decoded decode_one(std::string_view bytes) noexcept {
    assert(!bytes.empty() && "decode_one requires at least one byte");

    const auto b0 = static_cast<u8>(bytes[0]);
    if (b0 < 0x80) {
        return {b0, 1, DecodeStatus::OK};
    }
    if (b0 < 0xC2 || b0 > 0xF4) {
        // stray continuation byte, overlong 2-byte lead (C0/C1), or beyond
        // Unicode range (F5..FF): never starts a well-formed sequence
        return {k_replacement_codepoint, 1, DecodeStatus::INVALID};
    }

    // Well-formed ranges per Unicode Table 3-7. The second byte carries all
    // the irregular constraints (overlong / surrogate / out-of-range).
    u8 len = 2;
    u8 lo = 0x80;
    u8 hi = 0xBF;
    if (b0 >= 0xF0) {
        len = 4;
        if (b0 == 0xF0) {
            lo = 0x90; // reject overlong
        } else if (b0 == 0xF4) {
            hi = 0x8F; // reject > U+10FFFF
        }
    } else if (b0 >= 0xE0) {
        len = 3;
        if (b0 == 0xE0) {
            lo = 0xA0; // reject overlong
        } else if (b0 == 0xED) {
            hi = 0x9F; // reject surrogates U+D800..U+DFFF
        }
    }

    char32_t codepoint = b0 & (0x7F >> len);
    for (u8 i = 1; i < len; ++i) {
        if (i >= bytes.size()) {
            return {k_replacement_codepoint, static_cast<u8>(bytes.size()), DecodeStatus::INCOMPLETE};
        }
        const auto b = static_cast<u8>(bytes[i]);
        if (b < (i == 1 ? lo : 0x80) || b > (i == 1 ? hi : 0xBF)) {
            // maximal subpart: every byte before this one (Unicode 3.9)
            return {k_replacement_codepoint, i, DecodeStatus::INVALID};
        }
        codepoint = (codepoint << 6) | (b & 0x3F);
    }

    return {codepoint, len, DecodeStatus::OK};
}

Encoded encode_one(char32_t codepoint) noexcept {
    Encoded out{};
    if (codepoint < 0x80) {
        out.bytes[0] = static_cast<char>(codepoint);
        out.size = 1;
    } else if (codepoint < 0x800) {
        out.bytes[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out.bytes[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        out.size = 2;
    } else if (codepoint < 0x10000) {
        if (0xD800 <= codepoint && codepoint <= 0xDFFF) {
            return out; // surrogates are not scalar values
        }
        out.bytes[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out.bytes[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out.bytes[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        out.size = 3;
    } else if (codepoint <= 0x10FFFF) {
        out.bytes[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        out.bytes[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out.bytes[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out.bytes[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        out.size = 4;
    }
    return out;
}

usize find_invalid(std::string_view bytes) noexcept {
    usize i = 0;
    while (i < bytes.size()) {
        if (static_cast<u8>(bytes[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto decoded = decode_one(bytes.substr(i));
        if (decoded.status != DecodeStatus::OK) {
            return i;
        }
        i += decoded.size;
    }
    return std::string_view::npos;
}

usize complete_prefix_len(std::string_view bytes) noexcept {
    // A sequence still awaiting input is at most 3 bytes (4-byte lead plus
    // two continuations), so only the tail can be pending.
    const usize n = bytes.size();
    const usize scan = n < 3 ? n : 3;
    for (usize back = 1; back <= scan; ++back) {
        const auto b = static_cast<u8>(bytes[n - back]);
        if (b < 0x80) {
            return n; // ASCII never participates in a multi-byte sequence
        }
        if ((b & 0xC0) == 0x80) {
            continue; // continuation byte; keep looking for its lead
        }
        // lead byte: pending only if the tail is a valid proper prefix;
        // ill-formed tails are complete (sanitization will replace them)
        const auto decoded = decode_one(bytes.substr(n - back));
        return decoded.status == DecodeStatus::INCOMPLETE ? n - back : n;
    }
    return n;
}

void append_sanitized(std::string &out, std::string_view bytes) {
    usize valid_start = 0;
    usize i = 0;
    while (i < bytes.size()) {
        if (static_cast<u8>(bytes[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto decoded = decode_one(bytes.substr(i));
        if (decoded.status == DecodeStatus::OK) {
            i += decoded.size;
            continue;
        }
        // INVALID consumes the maximal subpart; INCOMPLETE consumes the
        // truncated tail - one U+FFFD each
        out.append(bytes.substr(valid_start, i - valid_start));
        out.append(k_replacement);
        i += decoded.size;
        valid_start = i;
    }
    out.append(bytes.substr(valid_start));
}

std::string sanitize(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    append_sanitized(out, bytes);
    return out;
}

void Sanitizer::feed(std::string_view bytes, std::string &out) {
    if (bytes.empty()) {
        return;
    }

    if (carry_len > 0) {
        // Resolve the carried prefix against the new bytes. 4 new bytes are
        // always enough: from a valid prefix, decode_one either completes,
        // rejects a continuation, or stays INCOMPLETE only under 4 total.
        char buf[7];
        std::memcpy(buf, carry, carry_len);
        const usize take = bytes.size() < sizeof(buf) - carry_len ? bytes.size() : sizeof(buf) - carry_len;
        std::memcpy(buf + carry_len, bytes.data(), take);
        const std::string_view combined(buf, carry_len + take);

        const auto decoded = decode_one(combined);
        if (decoded.status == DecodeStatus::INCOMPLETE) {
            // all input consumed and still a proper prefix (< 4 bytes total)
            std::memcpy(carry, combined.data(), combined.size());
            carry_len = static_cast<u8>(combined.size());
            return;
        }
        if (decoded.status == DecodeStatus::OK) {
            out.append(combined.substr(0, decoded.size));
        } else {
            out.append(k_replacement);
        }
        // the carry was a valid prefix, so decode never stops short of it
        assert(decoded.size >= carry_len && "maximal subpart cannot end inside the carried prefix");
        bytes.remove_prefix(decoded.size - carry_len);
        carry_len = 0;
    }

    const usize complete = complete_prefix_len(bytes);
    append_sanitized(out, bytes.substr(0, complete));
    const usize tail = bytes.size() - complete;
    if (tail > 0) {
        std::memcpy(carry, bytes.data() + complete, tail);
    }
    carry_len = static_cast<u8>(tail);
}

void Sanitizer::finish(std::string &out) {
    if (carry_len > 0) {
        // a valid proper prefix that can no longer complete: one maximal
        // subpart, one U+FFFD - same as sanitize() on a truncated buffer
        out.append(k_replacement);
        carry_len = 0;
    }
}

} // namespace lighter::encoding::utf8
