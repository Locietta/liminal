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

class bound_client;

class Client : public detail::RequestSettings {
public:
    Client();
    ~Client();

    Client(const Client &) = default;
    Client &operator=(const Client &) = default;

    Client(Client &&) noexcept;
    Client &operator=(Client &&) noexcept;

    bound_client on(EventLoop &loop = EventLoop::current()) & noexcept;
    bound_client on(EventLoop &loop = EventLoop::current()) && noexcept;

private:
    friend class bound_client;
    std::shared_ptr<detail::SharedResources> shared;
};

} // namespace lighter::http
