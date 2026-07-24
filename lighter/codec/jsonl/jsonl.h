#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/json.hpp>
#include <glaze/json/ndjson.hpp>

#include <lighter/async/vocab/outcome.h>
#include <lighter/codec/json/json.h>
#include <lighter/types.hpp>

/// JSON Lines (ndjson): one JSON record per line. Homogeneous containers go
/// through to_string/parse; heterogeneous streams (e.g. session transcripts)
/// iterate lines() and decode each record with codec::json::parse<T>.
namespace lighter::codec::jsonl {

using json::Error;
using json::Result;

inline constexpr json::Opts k_write_opts{{.format = glz::NDJSON}};

/// Inputs are string_views over arbitrary buffers, not null-terminated.
inline constexpr json::Opts k_parse_opts{{.format = glz::NDJSON, .null_terminated = false}};

template <typename Container>
Result<std::string> to_string(const Container &values) {
    auto encoded = glz::write<k_write_opts>(values);
    if (!encoded) {
        return outcome_error(json::detail::from_glaze(encoded.error()));
    }
    return *std::move(encoded);
}

template <typename Container>
Result<void> parse(std::string_view text, Container &out) {
    if (auto ctx = glz::read<k_parse_opts>(out, text)) {
        return outcome_error(json::detail::from_glaze(ctx, text));
    }
    return {};
}

template <typename Container>
    requires std::default_initializable<Container>
Result<Container> parse(std::string_view text) {
    Container values{};
    auto parsed = parse(text, values);
    if (!parsed) {
        return outcome_error(std::move(parsed).error());
    }
    return values;
}

/// Encode one record and append it to `buffer` as a newline-terminated line.
/// Compact JSON never contains raw newlines, so the record stays on one line.
template <typename T>
Result<void> append(std::string &buffer, const T &value) {
    auto encoded = json::to_string(value);
    if (!encoded) {
        return outcome_error(std::move(encoded).error());
    }
    if (!buffer.empty() && !buffer.ends_with('\n')) {
        buffer.push_back('\n');
    }
    buffer += *encoded;
    buffer.push_back('\n');
    return {};
}

/// Zero-copy view over the non-empty lines of a jsonl buffer. Handles both
/// "\n" and "\r\n" endings and skips blank lines; each element is one record's
/// raw JSON, ready for codec::json::parse<T>.
struct Lines {
    std::string_view text;

    struct Sentinel {};

    struct Iterator {
        explicit Iterator(std::string_view text) : remaining(text) { advance(); }

        std::string_view operator*() const { return line; }

        Iterator &operator++() {
            advance();
            return *this;
        }

        bool operator==(Sentinel /*end*/) const { return done; }

    private:
        void advance() {
            while (!remaining.empty()) {
                auto pos = remaining.find('\n');
                std::string_view candidate;
                if (pos == std::string_view::npos) {
                    candidate = remaining;
                    remaining = {};
                } else {
                    candidate = remaining.substr(0, pos);
                    remaining.remove_prefix(pos + 1);
                }
                if (candidate.ends_with('\r')) {
                    candidate.remove_suffix(1);
                }
                if (!candidate.empty()) {
                    line = candidate;
                    return;
                }
            }
            done = true;
        }

        std::string_view remaining;
        std::string_view line;
        bool done = false;
    };

    Iterator begin() const { return Iterator(text); }

    Sentinel end() const { return {}; }
};

inline Lines lines(std::string_view text) { return {text}; }

} // namespace lighter::codec::jsonl
