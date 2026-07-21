#pragma once

#include <chrono>
#include <memory>
#include <utility>

#include <lighter/http/request_settings.h>
#include <lighter/async/io/loop.h>

namespace lighter::http::detail {

struct SharedResources;

} // namespace lighter::http::detail

namespace lighter::http {

struct BoundClient;

struct Client : detail::RequestSettings {

    Client();
    ~Client();

    Client(const Client &) = default;
    Client &operator=(const Client &) = default;

    Client(Client &&) noexcept;
    Client &operator=(Client &&) noexcept;

    BoundClient on(EventLoop &loop = EventLoop::current()) & noexcept;
    BoundClient on(EventLoop &loop = EventLoop::current()) && noexcept;

private:
    friend struct BoundClient;
    std::shared_ptr<detail::SharedResources> shared;
};

} // namespace lighter::http
