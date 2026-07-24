#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/vocab/outcome.h>
#include <lighter/http/common.h>
#include <lighter/types.hpp>

namespace lighter::http {

struct Response {
    int status = 0;
    std::string url;
    std::vector<Header> headers;
    std::vector<byte> body;

    bool ok() const noexcept { return 200 <= status && status < 300; }

    std::span<const byte> bytes() const noexcept;

    std::string_view text() const noexcept;

    std::string text_copy() const;

    /// Decode the response body as JSON. Defined in <lighter/http/json.h>.
    template <typename T>
    Outcome<T, Error, void> json() const;

    std::optional<std::string_view> header_value(std::string_view name) const noexcept;
};

} // namespace lighter::http
