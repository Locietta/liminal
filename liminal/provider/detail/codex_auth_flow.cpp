#include "codex_auth_flow.h"

#include <string_view>
#include <utility>

#include <glaze/json.hpp>

#include <lighter/async/runtime/task.h>
#include <lighter/codec/json/json.h>

namespace liminal::codex::detail {

namespace json = lighter::codec::json;
using lighter::fail;
using lighter::outcome_error;

struct JwtAuthClaim {
    std::string chatgpt_account_id;
};

struct JwtPayload {
    JwtAuthClaim auth;
};

} // namespace liminal::codex::detail

template <>
struct glz::meta<liminal::codex::detail::JwtPayload> {
    using T = liminal::codex::detail::JwtPayload;
    static constexpr auto value = object("https://api.openai.com/auth", &T::auth);
};

namespace liminal::codex::detail {

namespace {

constexpr json::Opts k_json_options{{.null_terminated = false, .error_on_unknown_keys = false}};

Result<std::string> decode_base64_url(std::string_view encoded) {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string normalized(encoded);
    for (auto &ch : normalized) {
        if (ch == '-') ch = '+';
        if (ch == '_') ch = '/';
    }
    while (normalized.size() % 4 != 0) normalized.push_back('=');

    std::string decoded;
    decoded.reserve(normalized.size() / 4 * 3);
    u32 accumulator = 0;
    int bits = 0;
    for (char ch : normalized) {
        if (ch == '=') break;
        const auto index = alphabet.find(ch);
        if (index == std::string_view::npos) {
            return outcome_error(Error::config("Codex access token has invalid base64url payload"));
        }
        accumulator = (accumulator << 6) | static_cast<u32>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return decoded;
}

Result<std::string> account_id_from_token(std::string_view token) {
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? first : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return outcome_error(Error::config("Codex access token is not a JWT"));
    }
    auto payload_text = decode_base64_url(token.substr(first + 1, second - first - 1));
    if (!payload_text) return outcome_error(std::move(payload_text).error());

    JwtPayload payload;
    if (auto parsed = glz::read<k_json_options>(payload, *payload_text)) {
        return outcome_error(Error::config("invalid Codex access token payload: " + glz::format_error(parsed, *payload_text)));
    }
    if (payload.auth.chatgpt_account_id.empty()) {
        return outcome_error(Error::config("Codex access token has no ChatGPT account ID"));
    }
    return std::move(payload.auth.chatgpt_account_id);
}

bool authorization_pending(const Error &error) {
    return error.kind == ErrorKind::HTTP_STATUS &&
           (error.status == 403 || error.status == 404 || error.detail.find("deviceauth_authorization_pending") != std::string::npos);
}

bool slow_down(const Error &error) { return error.kind == ErrorKind::HTTP_STATUS && error.detail.find("slow_down") != std::string::npos; }

provider::ResolvedAuth resolved(const Credentials &credentials) {
    return {
        .bearer_token = credentials.access_token,
        .headers =
            {
                {.name = "chatgpt-account-id", .value = credentials.account_id},
                {.name = "originator", .value = "liminal"},
                {.name = "OpenAI-Beta", .value = "responses=experimental"},
            },
    };
}

} // namespace

Result<Credentials> credentials_from_token(TokenResponse token, i64 now_unix_milliseconds) {
    if (token.access_token.empty() || token.refresh_token.empty() || token.expires_in <= 0) {
        return outcome_error(Error::config("Codex token response is missing required fields"));
    }
    auto account_id = account_id_from_token(token.access_token);
    if (!account_id) return outcome_error(std::move(account_id).error());
    return Credentials{
        .access_token = std::move(token.access_token),
        .refresh_token = std::move(token.refresh_token),
        .expires_at = now_unix_milliseconds + token.expires_in * 1000,
        .account_id = *std::move(account_id),
    };
}

lighter::Task<void, Error> login_device(const DeviceLoginAttempts &attempts, DeviceCodeNotice notice, std::chrono::seconds timeout) {
    auto device = co_await attempts.start().or_fail();
    if (device.verification_url.empty() || device.device_auth_id.empty() || device.user_code.empty() || device.interval.count() < 0) {
        co_await fail(Error::config("Codex device login response is missing required fields"));
    }

    notice(device.verification_url, device.user_code);
    const auto deadline = attempts.now() + timeout;
    std::optional<DeviceToken> authorized;
    while (attempts.now() < deadline) {
        co_await attempts.sleep(device.interval);
        auto polled = co_await attempts.poll(device);
        if (polled) {
            authorized = *std::move(polled);
            break;
        }
        auto error = std::move(polled).error();
        if (slow_down(error)) {
            device.interval += std::chrono::seconds(5);
            continue;
        }
        if (authorization_pending(error)) continue;
        co_await fail(std::move(error));
    }
    if (!authorized || authorized->authorization_code.empty() || authorized->code_verifier.empty()) {
        co_await fail(Error::config("Codex device login timed out"));
    }

    auto token = co_await attempts.exchange(*authorized).or_fail();
    auto credentials = co_await lighter::or_fail(credentials_from_token(std::move(token), attempts.now_unix_milliseconds()));
    co_await lighter::or_fail(attempts.save(credentials));
}

lighter::Task<provider::ResolvedAuth, Error> resolve_auth(const RefreshAttempts &attempts, Credentials &credentials,
                                                          std::chrono::milliseconds refresh_margin) {
    const auto now = attempts.now_unix_milliseconds();
    if (credentials.expires_at <= now + refresh_margin.count()) {
        auto token = co_await attempts.refresh(credentials.refresh_token).or_fail();
        credentials = co_await lighter::or_fail(credentials_from_token(std::move(token), now));
        co_await lighter::or_fail(attempts.save(credentials));
    }
    co_return resolved(credentials);
}

} // namespace liminal::codex::detail
