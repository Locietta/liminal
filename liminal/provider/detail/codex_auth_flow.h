#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <lighter/async/runtime/task.h>

#include "liminal/error.h"
#include "liminal/provider/auth.h"
#include "liminal/provider/codex_auth.h"

namespace liminal::codex::detail {

struct Credentials {
    std::string type = "oauth";
    std::string access_token;
    std::string refresh_token;
    i64 expires_at = 0;
    std::string account_id;
};

struct DeviceStart {
    std::string verification_url;
    std::string device_auth_id;
    std::string user_code;
    std::chrono::seconds interval;
};

struct DeviceToken {
    std::string authorization_code;
    std::string code_verifier;
};

struct TokenResponse {
    std::string access_token;
    std::string refresh_token;
    i64 expires_in = 0;
};

struct DeviceLoginAttempts {
    std::copyable_function<lighter::Task<DeviceStart, Error>() const> start;
    std::copyable_function<lighter::Task<DeviceToken, Error>(const DeviceStart &device) const> poll;
    std::copyable_function<lighter::Task<TokenResponse, Error>(const DeviceToken &device) const> exchange;
    std::copyable_function<lighter::Task<>(std::chrono::seconds delay) const> sleep;
    std::copyable_function<Result<void>(const Credentials &credentials) const> save;
    std::copyable_function<std::chrono::steady_clock::time_point() const> now;
    std::copyable_function<i64() const> now_unix_milliseconds;
};

struct RefreshAttempts {
    std::copyable_function<lighter::Task<TokenResponse, Error>(const std::string &refresh_token) const> refresh;
    std::copyable_function<Result<void>(const Credentials &credentials) const> save;
    std::copyable_function<i64() const> now_unix_milliseconds;
};

Result<Credentials> credentials_from_token(TokenResponse token, i64 now_unix_milliseconds);

lighter::Task<void, Error> login_device(const DeviceLoginAttempts &attempts, DeviceCodeNotice notice, std::chrono::seconds timeout);

lighter::Task<provider::ResolvedAuth, Error> resolve_auth(const RefreshAttempts &attempts, Credentials &credentials,
                                                          std::chrono::milliseconds refresh_margin);

} // namespace liminal::codex::detail
