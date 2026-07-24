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

namespace lighter::http::detail {

struct SharedResources {
    curl::ShareHandle share{};
};

std::shared_ptr<SharedResources> make_shared_resources();

} // namespace lighter::http::detail

namespace lighter::http {

struct Request;
struct BoundClient;
struct StreamingResponse;

} // namespace lighter::http

namespace lighter::http::detail {

struct InflightRequest;
struct InflightRequestState;
Task<Response, Error> execute_request(Request request, EventLoop &loop);
Task<StreamingResponse, Error> execute_stream_request(Request request, EventLoop &loop);

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

    /// Encode `value` as the JSON request body. Defined in <lighter/http/json.h>.
    template <typename T>
    Request &json(const T &value);

    Task<Response, Error> send() &;
    Task<Response, Error> send() &&;

    /// Start the transfer and resolve once the final response headers are in,
    /// leaving the body to be pulled from the StreamingResponse. Dropping the
    /// StreamingResponse (or cancelling) aborts the transfer.
    Task<StreamingResponse, Error> stream() &&;

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
