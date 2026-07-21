#include "bound_client.h"

#include <string>
#include <utility>

namespace lighter::http {

bound_client::bound_client(Client owner, EventLoop &loop) noexcept : owner(std::move(owner)), dispatch_loop(&loop) {}

http::Request bound_client::request(std::string method, std::string url) const noexcept {
    auto req = http::Request(owner, owner.shared, dispatch_loop);
    req.method(std::move(method));
    req.url_string = std::move(url);
    return req;
}

http::Request bound_client::get(std::string url) const noexcept { return request(std::string(http::method::get), std::move(url)); }

http::Request bound_client::post(std::string url) const noexcept { return request(std::string(http::method::post), std::move(url)); }

http::Request bound_client::put(std::string url) const noexcept { return request(std::string(http::method::put), std::move(url)); }

http::Request bound_client::patch(std::string url) const noexcept { return request(std::string(http::method::patch), std::move(url)); }

http::Request bound_client::del(std::string url) const noexcept { return request(std::string(http::method::del), std::move(url)); }

http::Request bound_client::head(std::string url) const noexcept { return request(std::string(http::method::head), std::move(url)); }

} // namespace lighter::http
