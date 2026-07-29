#pragma once

#include <array>
#include <contracts>
#include <string>
#include <string_view>

#include <lighter/types.hpp>

/// UTF-8 primitives: validation, decoding, and sanitization with the Unicode
/// "maximal subpart" replacement policy (one U+FFFD per maximal subpart of an
/// ill-formed sequence; Unicode 3.9, matches the WHATWG encoding spec).
/// Locale-free, allocation-free except where a std::string is produced.
///
/// lighter strings are always UTF-8 bytes. These helpers police the borders
/// where foreign bytes enter: HTTP bodies, process output, files. Non-Unicode
/// encodings (GBK, Shift_JIS, ...) will be handled by a separate streaming
/// transcoder in this module; everything here is UTF-8 only.
namespace lighter::encoding::utf8 {

/// U+FFFD REPLACEMENT CHARACTER, encoded.
inline constexpr std::string_view k_replacement = "\xEF\xBF\xBD";

inline constexpr char32_t k_replacement_codepoint = 0xFFFD;

enum struct DecodeStatus : u8 {
    OK,         ///< a well-formed sequence was decoded
    INVALID,    ///< ill-formed bytes; `size` is the maximal subpart length
    INCOMPLETE, ///< the input ends with a valid proper prefix of a sequence
};

struct Decoded {
    char32_t codepoint; ///< decoded scalar value; U+FFFD unless OK
    u8 size;            ///< bytes consumed; for INCOMPLETE, all remaining bytes
    DecodeStatus status;
};

/// Decode the sequence starting at bytes[0]. `bytes` must be non-empty.
/// INVALID consumes exactly the maximal subpart (>= 1 byte), so repeated
/// calls implement per-subpart U+FFFD replacement.
Decoded decode_one(std::string_view bytes) noexcept pre(!bytes.empty());

struct Encoded {
    std::array<char, 4> bytes;
    u8 size; ///< 0 for invalid scalar values (surrogates, > U+10FFFF)

    std::string_view view() const noexcept { return {bytes.data(), size}; }
};

/// Encode one Unicode scalar value.
Encoded encode_one(char32_t codepoint) noexcept;

/// Offset of the first ill-formed byte, or npos when the whole input is
/// well-formed. A truncated trailing sequence counts as ill-formed here;
/// use complete_prefix_len() when more input may still arrive.
usize find_invalid(std::string_view bytes) noexcept;

inline bool is_valid(std::string_view bytes) noexcept { return find_invalid(bytes) == std::string_view::npos; }

/// Length of the longest prefix that does not end inside a multi-byte
/// sequence. The (at most 3) bytes past the returned offset are a valid
/// proper prefix that later input may complete; ill-formed bytes never count
/// as pending. This is the streaming split point: emit bytes[0..len), carry
/// the rest.
usize complete_prefix_len(std::string_view bytes) noexcept;

/// Append `bytes` to `out`, replacing each maximal ill-formed subpart
/// (including a truncated trailing sequence) with U+FFFD.
void append_sanitized(std::string &out, std::string_view bytes);

/// Copy of `bytes` with every maximal ill-formed subpart replaced by U+FFFD.
/// The result is always well-formed UTF-8.
std::string sanitize(std::string_view bytes);

/// Incremental sanitizer for byte streams chunked at arbitrary boundaries
/// (TCP, pipes, curl bursts). feed() emits only whole sequences - a trailing
/// incomplete sequence is carried until later input completes it - and
/// replaces ill-formed bytes with U+FFFD exactly like sanitize(). finish()
/// flushes a carried tail that can no longer complete as one U+FFFD.
///
/// Feeding the same bytes under any chunking yields byte-identical output to
/// sanitize() over the whole stream.
struct Sanitizer {
    void feed(std::string_view bytes, std::string &out);

    void finish(std::string &out);

    /// Bytes held back waiting for the rest of a sequence.
    usize pending() const noexcept { return carry_len; }

private:
    char carry[3] = {}; // a valid proper prefix is at most 3 bytes
    u8 carry_len = 0;
};

} // namespace lighter::encoding::utf8
