#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/types.hpp>

/// Streaming conversion between character encodings (iconv backend). The
/// dominant direction is X -> UTF-8: decoding legacy-encoded HTTP bodies,
/// files, and subprocess output (GBK on Chinese Windows, CP932 on Japanese)
/// into lighter's always-UTF-8 strings.
namespace lighter::encoding {

/// Resolve a charset label - as found in HTTP Content-Type, HTML meta, or
/// user config - to a canonical iconv encoding name. Follows a pragmatic
/// subset of the WHATWG Encoding Standard's label table, including its
/// web-reality quirks (latin1/us-ascii mean windows-1252, shift_jis means
/// CP932, euc-kr means UHC). Matching is case-insensitive and ignores
/// surrounding ASCII whitespace. Returns nullopt for unknown labels.
std::optional<std::string_view> resolve_label(std::string_view label) noexcept;

/// Incremental transcoder with the same feed/finish shape as utf8::Sanitizer:
/// feed() converts whole characters only - a sequence torn at a chunk
/// boundary is carried until later input completes it - and finish() flushes
/// a dangling tail as a replacement character, so any chunking of a stream
/// yields byte-identical output.
///
/// Invalid input bytes become the replacement character (U+FFFD when the
/// target is Unicode, '?' otherwise) and conversion continues, mirroring the
/// utf8::Sanitizer policy; feed() itself never fails. Unconvertible-but-valid
/// input (e.g. emoji into GBK) is replaced the same way.
///
/// After finish() the conversion state is reset, so one Transcoder can be
/// reused for a new stream of the same encoding pair.
struct Transcoder {
    /// `from`/`to` accept charset labels (resolve_label) as well as raw
    /// iconv encoding names. Fails when iconv does not support the pair.
    static Result<Transcoder> open(std::string_view from, std::string_view to = "UTF-8");

    Transcoder(Transcoder &&other) noexcept;
    Transcoder &operator=(Transcoder &&other) noexcept;
    Transcoder(const Transcoder &) = delete;
    Transcoder &operator=(const Transcoder &) = delete;

    ~Transcoder();

    /// Convert `bytes`, appending the result to `out`. A trailing incomplete
    /// sequence is carried into the next feed() call.
    void feed(std::string_view bytes, std::string &out);

    /// Flush: a carried tail that can no longer complete becomes one
    /// replacement character, and stateful target encodings (ISO-2022-JP)
    /// emit their closing shift sequence. Resets the state for reuse.
    void finish(std::string &out);

    /// Bytes held back waiting for the rest of a sequence.
    usize pending() const noexcept { return carry.size(); }

private:
    Transcoder() = default;

    /// Convert as much of [in, in + in_left) as possible into `out`,
    /// replacing invalid input. On return in_left > 0 means a trailing
    /// incomplete sequence remains for the caller to carry.
    void run(const char *&in, usize &in_left, std::string &out);

    void *cd = nullptr; // iconv_t, kept opaque to avoid leaking <iconv.h>
    std::string carry;
    std::string replacement;
    bool utf8_input = false; // enables whole-sequence skips on invalid input
};

/// One-shot convenience: decode a whole buffer into UTF-8.
Result<std::string> to_utf8(std::string_view bytes, std::string_view from);

} // namespace lighter::encoding
