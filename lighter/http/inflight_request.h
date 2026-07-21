#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <lighter/http/curl.h>
#include <lighter/http/request.h>
#include <lighter/http/response.h>
#include <lighter/async/vocab/error.h>
#include <lighter/async/vocab/outcome.h>

namespace lighter::http::detail {

struct InflightRequest : RequestSettings {

    explicit InflightRequest(http::Request request) noexcept;
    InflightRequest(const InflightRequest &) = delete;
    InflightRequest &operator=(const InflightRequest &) = delete;
    InflightRequest(InflightRequest &&) = delete;
    InflightRequest &operator=(InflightRequest &&) = delete;

    bool fail(Error err) noexcept;
    bool fail(curl::EasyError code) noexcept;
    bool prepare() noexcept;
    bool bind_runtime(void *opaque) noexcept;
    void clear_runtime_binding() noexcept;
    Outcome<Response, Error, Cancellation> finish() noexcept;

    static std::size_t on_write(char *data, std::size_t size, std::size_t count, void *userdata);
    static std::size_t on_header(char *data, std::size_t size, std::size_t count, void *userdata);

    std::shared_ptr<SharedResources> shared;
    std::string method_name;
    std::string url_string;
    std::vector<QueryParam> query_params;
    std::string body_text;
    curl::EasyHandle easy;
    curl::SList header_lines;
    Response out{};
    Error result{};
    std::string final_url;

private:
    bool apply_url() noexcept;
    bool apply_method() noexcept;
    bool apply_body() noexcept;
    bool apply_headers() noexcept;
    bool apply_cookies() noexcept;
    bool apply_user_agent() noexcept;
    bool apply_redirect() noexcept;
    bool apply_tls() noexcept;
    bool apply_proxy() noexcept;
    bool apply_timeout() noexcept;
    bool apply_curl_options() noexcept;
};

template <typename T> bool easy_setopt(InflightRequest &request, CURLoption option, T value) noexcept {
    if (auto err = curl::setopt(request.easy.get(), option, value); !curl::ok(err)) {
        return request.fail(err);
    }
    return true;
}

} // namespace lighter::http::detail
