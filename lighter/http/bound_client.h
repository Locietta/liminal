#pragma once

#include <string>
#include <utility>

#include <lighter/http/client.h>
#include <lighter/http/request.h>
#include <lighter/http/response.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>

namespace lighter::http {

struct BoundClient {

    BoundClient(Client owner, EventLoop &loop) noexcept;

    http::Request request(std::string method, std::string url) const noexcept;
    http::Request get(std::string url) const noexcept;
    http::Request post(std::string url) const noexcept;
    http::Request put(std::string url) const noexcept;
    http::Request patch(std::string url) const noexcept;
    http::Request del(std::string url) const noexcept;
    http::Request head(std::string url) const noexcept;

    EventLoop &loop() const noexcept { return *dispatch_loop; }

private:
    Client owner;
    EventLoop *dispatch_loop = nullptr;
};

} // namespace lighter::http
