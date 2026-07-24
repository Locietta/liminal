#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <lighter/http/curl.h>
#include <lighter/types.hpp>

namespace lighter::http {

namespace method {

constexpr inline std::string_view get = "GET";
constexpr inline std::string_view post = "POST";
constexpr inline std::string_view put = "PUT";
constexpr inline std::string_view patch = "PATCH";
constexpr inline std::string_view del = "DELETE";
constexpr inline std::string_view head = "HEAD";
constexpr inline std::string_view options = "OPTIONS";
constexpr inline std::string_view trace = "TRACE";
constexpr inline std::string_view connect = "CONNECT";

} // namespace method

enum struct ErrorKind {
    CURL,
    INVALID_REQUEST,
    JSON_ENCODE,
    JSON_DECODE,
};

struct Error {
    ErrorKind kind = ErrorKind::CURL;
    curl::EasyError curl_code = CURLE_OK;
    std::string detail;

    static Error from_curl(curl::EasyError code, std::string detail = {}) {
        return {.kind = ErrorKind::CURL, .curl_code = code, .detail = std::move(detail)};
    }

    static Error from_curl(curl::MultiError code, std::string detail = {}) {
        return from_curl(curl::to_easy_error(code), std::move(detail));
    }

    static Error from_curl(curl::ShareError code, std::string detail = {}) {
        return from_curl(curl::to_easy_error(code), std::move(detail));
    }

    static Error invalid_request(std::string detail) { return {.kind = ErrorKind::INVALID_REQUEST, .detail = std::move(detail)}; }

    static Error json_encode(std::string detail) { return {.kind = ErrorKind::JSON_ENCODE, .detail = std::move(detail)}; }

    static Error json_decode(std::string detail) { return {.kind = ErrorKind::JSON_DECODE, .detail = std::move(detail)}; }

    std::string message() const;
};

std::string message(const Error &err);

struct Header {
    std::string name;
    std::string value;
};

struct QueryParam {
    std::string name;
    std::string value;
};

using curl_option_hook = std::function<curl::EasyError(CURL *)>;

struct Proxy {
    std::string url;
    std::string username = {};
    std::string password = {};
};

struct RedirectPolicy {
    bool follow = true;
    usize max_redirects = 10;
    bool referer = true;

    static RedirectPolicy none() noexcept { return {.follow = false, .max_redirects = 0, .referer = false}; }

    static RedirectPolicy limited(usize max_redirects = 10) noexcept {
        return {.follow = true, .max_redirects = max_redirects, .referer = true};
    }
};

enum struct TlsVersion {
    TLS1_0,
    TLS1_1,
    TLS1_2,
    TLS1_3,
};

struct TlsOptions {
    bool https_only = false;
    bool danger_accept_invalid_certs = false;
    bool danger_accept_invalid_hostnames = false;
    std::optional<std::string> ca_file;
    std::optional<std::string> ca_path;
    std::optional<TlsVersion> min_version;
    std::optional<TlsVersion> max_version;
};

} // namespace lighter::http
