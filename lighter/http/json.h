#pragma once

/// Typed JSON support for http requests/responses. Kept out of request.h /
/// response.h so glaze is only pulled into TUs that actually use it.

#include <string>
#include <utility>

#include <lighter/codec/json/json.h>
#include <lighter/http/request.h>
#include <lighter/http/response.h>

namespace lighter::http {

template <typename T>
Request &Request::json(const T &value) {
    auto encoded = codec::json::to_string(value);
    if (!encoded) {
        remember_error(Error::json_encode(std::string(encoded.error().message())));
        return *this;
    }
    return json_text(*std::move(encoded));
}

template <typename T>
Outcome<T, Error, void> Response::json() const {
    auto decoded = codec::json::parse<T>(text());
    if (!decoded) {
        return outcome_error(Error::json_decode(std::string(decoded.error().message())));
    }
    return *std::move(decoded);
}

} // namespace lighter::http
