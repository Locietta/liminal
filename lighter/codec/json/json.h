#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/json.hpp>

#include <lighter/async/vocab/outcome.h>
#include <lighter/types.hpp>

namespace lighter::codec::json {

struct Error {
    glz::error_code code = glz::error_code::none;
    std::string detail;

    std::string_view message() const noexcept { return detail; }
};

template <typename T>
using Result = Outcome<T, Error, void>;

namespace detail {

inline Error from_glaze(const glz::error_ctx &ctx) { return {.code = ctx.ec, .detail = glz::format_error(ctx)}; }

inline Error from_glaze(const glz::error_ctx &ctx, std::string_view buffer) {
    return {.code = ctx.ec, .detail = glz::format_error(ctx, buffer)};
}

} // namespace detail

/// glz::opts extension: extra knobs are opt-in members on a derived struct.
struct Opts : glz::opts {
    /// serialize/parse enums by reflected name (P2996) instead of integer value
    bool reflect_enums = true;
};

inline constexpr Opts k_write_opts{};

/// Inputs are string_views over arbitrary buffers (e.g. http bodies), not null-terminated.
inline constexpr Opts k_parse_opts{{.null_terminated = false}};

template <typename T>
Result<std::string> to_string(const T &value) {
    auto encoded = glz::write<k_write_opts>(value);
    if (!encoded) {
        return outcome_error(detail::from_glaze(encoded.error()));
    }
    return *std::move(encoded);
}

template <typename T>
Result<void> parse(std::string_view text, T &out) {
    if (auto ctx = glz::read<k_parse_opts>(out, text)) {
        return outcome_error(detail::from_glaze(ctx, text));
    }
    return {};
}

template <typename T>
    requires std::default_initializable<T>
Result<T> parse(std::string_view text) {
    T value{};
    auto parsed = parse(text, value);
    if (!parsed) {
        return outcome_error(std::move(parsed).error());
    }
    return value;
}

inline std::string prettify(std::string_view text) { return glz::prettify_json(text); }

inline std::string minify(std::string_view text) {
    // glz::minify_json requires a resizable input buffer
    std::string buffer(text);
    return glz::minify_json(buffer);
}

} // namespace lighter::codec::json
