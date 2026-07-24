#include "response.h"

#include <span>
#include <string>
#include <string_view>

#include <lighter/http/util.h>

namespace lighter::http {

std::string Error::message() const { return http::message(*this); }

std::string message(const Error &err) {
    switch (err.kind) {
        case ErrorKind::CURL:
            if (!err.detail.empty()) {
                return err.detail;
            }
            return std::string(curl::message(err.curl_code));
        case ErrorKind::INVALID_REQUEST: return err.detail.empty() ? std::string("invalid http request") : err.detail;
        case ErrorKind::JSON_ENCODE: return err.detail.empty() ? std::string("json encode failed") : err.detail;
        case ErrorKind::JSON_DECODE: return err.detail.empty() ? std::string("json decode failed") : err.detail;
    }
    return "unknown http error";
}

std::span<const byte> Response::bytes() const noexcept { return body; }

std::string_view Response::text() const noexcept {
    if (body.empty()) {
        return {};
    }
    auto *ptr = reinterpret_cast<const char *>(body.data());
    return std::string_view(ptr, body.size());
}

std::string Response::text_copy() const {
    auto view = text();
    return std::string(view.data(), view.size());
}

std::optional<std::string_view> Response::header_value(std::string_view name) const noexcept {
    for (const auto &item : headers) {
        if (detail::iequals(item.name, name)) {
            return item.value;
        }
    }
    return std::nullopt;
}

} // namespace lighter::http
