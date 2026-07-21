#include "client.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#include <lighter/http/bound_client.h>
#include <lighter/http/runtime.h>

namespace lighter::http {

namespace {

std::shared_ptr<detail::SharedResources> require_shared_resources() {
    auto resources = detail::make_shared_resources();
    if (!resources) {
        std::fprintf(stderr, "fatal: failed to initialize curl shared resources\n");
        std::abort();
    }
    return resources;
}

} // namespace

std::shared_ptr<detail::SharedResources> detail::make_shared_resources() {
    if (auto code = detail::ensure_curl_runtime(); !curl::ok(code)) {
        return {};
    }

    auto resources = std::make_shared<detail::SharedResources>();
    resources->share = curl::ShareHandle::create();
    if (!resources->share) {
        return {};
    }

    if (auto err = curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE); !curl::ok(err)) {
        return {};
    }

    if (auto err = curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS); !curl::ok(err)) {
        return {};
    }

    // SSL session sharing is best-effort: skip if curl was built without SSL.
    curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

    return resources;
}

Client::Client() : shared(require_shared_resources()) {}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;

Client &Client::operator=(Client &&) noexcept = default;

bound_client Client::on(EventLoop &loop) & noexcept { return bound_client(*this, loop); }

bound_client Client::on(EventLoop &loop) && noexcept { return bound_client(std::move(*this), loop); }

} // namespace lighter::http
