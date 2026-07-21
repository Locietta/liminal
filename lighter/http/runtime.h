#pragma once

#include <memory>

#include <lighter/http/curl.h>

namespace lighter::http {

struct request;

} // namespace lighter::http

namespace lighter::http::detail {

struct InflightRequest;
struct InflightRequestState;
using InflightRequestRef = std::shared_ptr<InflightRequestState>;

curl::EasyError ensure_curl_runtime() noexcept;

InflightRequestRef make_inflight_request_state(http::request request) noexcept;

void *inflight_request_opaque(const InflightRequestRef &request) noexcept;

InflightRequestRef retain_inflight_request(void *opaque) noexcept;

void mark_inflight_request_removed(const InflightRequestRef &request) noexcept;

void complete_inflight_request(const InflightRequestRef &request, curl::EasyError result, bool resume_inline) noexcept;

} // namespace lighter::http::detail
