#include "request.h"

#include <format>
#include <utility>

namespace lighter::http {

namespace {

Task<Response, Error> make_failed_response(Error err) { co_await fail(std::move(err)); }

} // namespace

Request::Request(std::shared_ptr<detail::SharedResources> shared, EventLoop *dispatch_loop) noexcept
    : shared(std::move(shared)), dispatch_loop(dispatch_loop) {}

Request::Request(const detail::RequestSettings &settings, std::shared_ptr<detail::SharedResources> shared,
                 EventLoop *dispatch_loop) noexcept
    : detail::RequestSettings(settings), shared(std::move(shared)), dispatch_loop(dispatch_loop) {}

Request &Request::query(std::string name, std::string value) {
    query_params.push_back({std::move(name), std::move(value)});
    return *this;
}

Request &Request::method(std::string value) {
    method_name = std::move(value);
    return *this;
}

Request &Request::bearer_auth(std::string token) { return header("authorization", std::format("Bearer {}", token)); }

Request &Request::basic_auth(std::string username, std::string password) {
    return header("authorization", std::format("Basic {}", detail::base64_encode(std::format("{}:{}", username, password))));
}

Request &Request::json_text(std::string body) {
    body_text = std::move(body);
    header("content-type", "application/json");
    return *this;
}

Request &Request::form(std::vector<QueryParam> fields) {
    body_text = detail::encode_pairs(fields);
    header("content-type", "application/x-www-form-urlencoded");
    return *this;
}

Request &Request::body(std::string body) {
    body_text = std::move(body);
    return *this;
}

Task<Response, Error> Request::send() & {
    if (staged_error) {
        return failed(*staged_error);
    }

    if (!dispatch_loop) {
        return failed(Error::invalid_request("request::send requires a bound_client"));
    }

    return detail::execute_request(*this, *dispatch_loop);
}

Task<Response, Error> Request::send() && {
    if (staged_error) {
        return failed(std::move(*staged_error));
    }

    if (!dispatch_loop) {
        return failed(Error::invalid_request("request::send requires a bound_client"));
    }

    return detail::execute_request(std::move(*this), *dispatch_loop);
}

Task<Response, Error> Request::failed(Error err) { return make_failed_response(std::move(err)); }

void Request::remember_error(Error err) noexcept {
    if (!staged_error) {
        staged_error = std::move(err);
    }
}

} // namespace lighter::http
