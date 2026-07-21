#include "bound_client.h"

#include <string>
#include <utility>

namespace lighter::http {

BoundClient::BoundClient(Client owner, EventLoop &loop) noexcept : owner(std::move(owner)), dispatch_loop(&loop) {}

http::Request BoundClient::request(std::string method, std::string url) const noexcept {
    auto req = http::Request(owner, owner.shared, dispatch_loop);
    req.method(std::move(method));
    req.url_string = std::move(url);
    return req;
}

http::Request BoundClient::get(std::string url) const noexcept { return request(std::string(http::method::get), std::move(url)); }

http::Request BoundClient::post(std::string url) const noexcept { return request(std::string(http::method::post), std::move(url)); }

http::Request BoundClient::put(std::string url) const noexcept { return request(std::string(http::method::put), std::move(url)); }

http::Request BoundClient::patch(std::string url) const noexcept { return request(std::string(http::method::patch), std::move(url)); }

http::Request BoundClient::del(std::string url) const noexcept { return request(std::string(http::method::del), std::move(url)); }

http::Request BoundClient::head(std::string url) const noexcept { return request(std::string(http::method::head), std::move(url)); }

} // namespace lighter::http
