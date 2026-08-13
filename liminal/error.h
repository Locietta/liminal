#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <lighter/async/vocab/error.h>
#include <lighter/codec/json/json.h>
#include <lighter/http/common.h>
#include <lighter/types.hpp>

namespace liminal {

using namespace lighter::types;

enum struct ErrorKind {
    CONFIG,      ///< missing/invalid configuration (e.g. no API key)
    HTTP,        ///< transport-level failure (curl, connect, TLS, mid-stream drop)
    HTTP_STATUS, ///< non-2xx response from the API
    JSON,        ///< failed to encode/decode JSON
    PROTOCOL,    ///< stream violated the Messages API event protocol
    TOOL,        ///< tool infrastructure failure (spawn, bad input, ...)
    STORAGE,     ///< durable session state or lease failure
};

enum struct ErrorCode {
    NONE,
    NOT_FOUND,
    IO,
    INVALID_DATA,
    AUTH_NOT_CONFIGURED,
    AUTH_INVALID,
};

struct Error {
    ErrorKind kind = ErrorKind::PROTOCOL;
    ErrorCode code = ErrorCode::NONE;
    std::string detail;

    // HTTP_STATUS extras
    int status = 0;
    std::string api_type;   ///< provider error envelope type, e.g. "overloaded_error"
    std::string request_id; ///< request-id header when available
    std::optional<std::chrono::milliseconds> retry_after;
    /// For API errors without an HTTP status (SSE error events): whether the
    /// originating provider classified its envelope type as transient. Only
    /// the provider knows its own vocabulary, so it sets this at creation.
    bool transient = false;

    // wrapped source errors
    std::optional<lighter::http::Error> http_error;
    std::optional<lighter::codec::json::Error> json_error;

    /// True if the failure is transient and the request may be re-sent verbatim.
    /// Callers must additionally ensure no output was already shown to the user.
    bool retryable() const noexcept {
        switch (kind) {
            case ErrorKind::HTTP: return true;
            case ErrorKind::HTTP_STATUS:
                if (status != 0) {
                    return status == 429 || status >= 500;
                }
                // SSE `error` events carry no HTTP status; the provider
                // classified its envelope type when it created the error.
                return transient;
            default: return false;
        }
    }

    std::string message() const {
        std::string out;
        switch (kind) {
            case ErrorKind::CONFIG: out = "config error: "; break;
            case ErrorKind::HTTP: out = "http error: "; break;
            case ErrorKind::HTTP_STATUS: out = "api error (status " + std::to_string(status) + "): "; break;
            case ErrorKind::JSON: out = "json error: "; break;
            case ErrorKind::PROTOCOL: out = "protocol error: "; break;
            case ErrorKind::TOOL: out = "tool error: "; break;
            case ErrorKind::STORAGE: out = "session storage error: "; break;
        }
        if (!api_type.empty()) {
            out += api_type + ": ";
        }
        out += detail;
        if (http_error) {
            out += " (" + http_error->message() + ")";
        }
        if (json_error) {
            out += " (";
            out += json_error->message();
            out += ")";
        }
        if (!request_id.empty()) {
            out += " [request-id: " + request_id + "]";
        }
        return out;
    }

    static Error config(std::string detail, ErrorCode code = ErrorCode::NONE) {
        return {.kind = ErrorKind::CONFIG, .code = code, .detail = std::move(detail)};
    }

    static Error http(lighter::http::Error error) {
        return {.kind = ErrorKind::HTTP, .detail = "transport failure", .http_error = std::move(error)};
    }

    static Error http_status(int status, std::string api_type, std::string detail, std::string request_id,
                             std::optional<std::chrono::milliseconds> retry_after = {}) {
        return {
            .kind = ErrorKind::HTTP_STATUS,
            .detail = std::move(detail),
            .status = status,
            .api_type = std::move(api_type),
            .request_id = std::move(request_id),
            .retry_after = retry_after,
        };
    }

    /// A provider-reported error with no HTTP status (e.g. an SSE `error`
    /// event); `transient` is the provider's own classification of its
    /// envelope type.
    static Error api(std::string api_type, std::string detail, std::string request_id, bool transient) {
        return {
            .kind = ErrorKind::HTTP_STATUS,
            .detail = std::move(detail),
            .api_type = std::move(api_type),
            .request_id = std::move(request_id),
            .transient = transient,
        };
    }

    static Error json(lighter::codec::json::Error error, std::string context = {}) {
        return {.kind = ErrorKind::JSON, .detail = std::move(context), .json_error = std::move(error)};
    }

    static Error protocol(std::string detail) { return {.kind = ErrorKind::PROTOCOL, .detail = std::move(detail)}; }

    static Error tool(std::string detail) { return {.kind = ErrorKind::TOOL, .detail = std::move(detail)}; }
    static Error storage(std::string detail) { return {.kind = ErrorKind::STORAGE, .detail = std::move(detail)}; }
};

template <typename T>
using Result = lighter::Outcome<T, Error, void>;

} // namespace liminal
