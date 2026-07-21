#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lighter/http/request_settings.h>
#include <lighter/http/response.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>

#if __has_include(<simdjson.h>)
#include "lighter/codec/json/json.h"
#define LIGHTER_HTTP_HAS_CODEC_JSON 1
#else
#define LIGHTER_HTTP_HAS_CODEC_JSON 0
#endif

namespace lighter::http::detail {

struct SharedResources {
    curl::ShareHandle share{};
};

std::shared_ptr<SharedResources> make_shared_resources();

} // namespace lighter::http::detail

namespace lighter::http {

struct Request;
struct BoundClient;

} // namespace lighter::http

namespace lighter::http::detail {

struct InflightRequest;
Task<Response, Error> execute_request(Request request, EventLoop &loop);

} // namespace lighter::http::detail

namespace lighter::http {

struct Request : detail::RequestSettings {

    Request() = delete;

    Request(std::shared_ptr<detail::SharedResources> shared, EventLoop *dispatch_loop) noexcept;
    Request(const detail::RequestSettings &settings, std::shared_ptr<detail::SharedResources> shared, EventLoop *dispatch_loop) noexcept;

    Request(const Request &) noexcept = default;
    Request &operator=(const Request &) noexcept = default;
    Request(Request &&) noexcept = default;
    Request &operator=(Request &&) noexcept = default;
    ~Request() = default;

    Request &query(std::string name, std::string value);
    Request &method(std::string value);
    Request &bearer_auth(std::string token);
    Request &basic_auth(std::string username, std::string password);
    Request &json_text(std::string body);
    Request &form(std::vector<QueryParam> fields);
    Request &body(std::string body);

    Task<Response, Error> send() &;
    Task<Response, Error> send() &&;

#if LIGHTER_HTTP_HAS_CODEC_JSON
    template <typename T> request &json(const T &value) {
        auto encoded = codec::json::to_string(value);
        if (!encoded) {
            remember_error(Error{ErrorKind::JSON_ENCODE, encoded.error().to_string()});
            return *this;
        }

        json_text(std::move(*encoded));
        return *this;
    }
#endif

private:
    friend struct BoundClient;
    friend struct detail::InflightRequest;
    void remember_error(Error err) noexcept;
    static Task<Response, Error> failed(Error err);

    std::shared_ptr<detail::SharedResources> shared;
    EventLoop *dispatch_loop = nullptr;
    std::string method_name = std::string(http::method::get);
    std::string url_string;
    std::vector<QueryParam> query_params;
    std::string body_text;
    std::optional<Error> staged_error;
};

} // namespace lighter::http
