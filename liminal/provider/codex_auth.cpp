#include "codex_auth.h"

#include <chrono>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/async/async.h>
#include <lighter/codec/json/json.h>
#include <lighter/http/http.h>

namespace liminal::codex {

namespace http = lighter::http;
namespace json = lighter::codec::json;
using lighter::fail;
using lighter::outcome_error;
using lighter::Task;

namespace {

constexpr std::string_view k_client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr std::string_view k_default_auth_base_url = "https://auth.openai.com";
constexpr std::string_view k_device_redirect_uri = "https://auth.openai.com/deviceauth/callback";
constexpr auto k_login_timeout = std::chrono::minutes(15);
constexpr auto k_refresh_margin = std::chrono::minutes(1);

struct Credentials {
    std::string type = "oauth";
    std::string access_token;
    std::string refresh_token;
    i64 expires_at = 0;
    std::string account_id;
};

struct AuthFile {
    std::optional<Credentials> codex;
};

struct DeviceStart {
    std::string device_auth_id;
    std::string user_code;
    std::variant<u64, std::string> interval;
};

struct DeviceStartRequest {
    std::string client_id;
};

struct DeviceTokenRequest {
    std::string device_auth_id;
    std::string user_code;
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

struct JwtAuthClaim {
    std::string chatgpt_account_id;
};

struct JwtPayload {
    JwtAuthClaim auth;
};

struct AuthState {
    Credentials credentials;
    std::filesystem::path path;
    std::string auth_base_url;

    Task<provider::ResolvedAuth, Error> resolve();
};

} // namespace

} // namespace liminal::codex

template <>
struct glz::meta<liminal::codex::JwtPayload> {
    using T = liminal::codex::JwtPayload;
    static constexpr auto value = object("https://api.openai.com/auth", &T::auth);
};

namespace liminal::codex {

namespace {

constexpr json::Opts k_json_options{{.null_terminated = false, .error_on_unknown_keys = false}};

std::string auth_base_url() {
    if (const char *value = std::getenv("LIMINAL_CODEX_AUTH_BASE_URL"); value && *value) {
        return value;
    }
    return std::string(k_default_auth_base_url);
}

template <typename T>
Result<T> parse_json(std::string_view text, std::string_view context) {
    T value{};
    if (auto result = glz::read<k_json_options>(value, text)) {
        return outcome_error(Error::config(std::string(context) + ": " + glz::format_error(result, text)));
    }
    return value;
}

Result<AuthFile> read_auth_file(const std::filesystem::path &path) {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return outcome_error(Error::config("cannot inspect auth file '" + path.string() + "': " + exists_error.message()));
    }
    if (!exists) {
        return AuthFile{};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return outcome_error(Error::config("cannot open auth file '" + path.string() + "'"));
    }
    std::string text(std::istreambuf_iterator<char>(input), {});
    return parse_json<AuthFile>(text, "invalid auth file '" + path.string() + "'");
}

Result<void> write_auth_file(const std::filesystem::path &path, Credentials credentials) {
    std::error_code directory_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
    }
    if (directory_error) {
        return outcome_error(
            Error::config("cannot create auth directory '" + path.parent_path().string() + "': " + directory_error.message()));
    }
    AuthFile file{.codex = std::move(credentials)};
    auto encoded = json::to_string(file);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "auth file"));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << *encoded;
    if (!output) {
        return outcome_error(Error::config("cannot write auth file '" + path.string() + "'"));
    }
#ifndef _WIN32
    std::error_code permission_error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, permission_error);
    if (permission_error) {
        return outcome_error(Error::config("cannot secure auth file '" + path.string() + "': " + permission_error.message()));
    }
#endif
    return {};
}

i64 unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

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
    auto payload = parse_json<JwtPayload>(*payload_text, "invalid Codex access token payload");
    if (!payload) return outcome_error(std::move(payload).error());
    if (payload->auth.chatgpt_account_id.empty()) {
        return outcome_error(Error::config("Codex access token has no ChatGPT account ID"));
    }
    return std::move(payload->auth.chatgpt_account_id);
}

Result<Credentials> credentials_from_token(TokenResponse token) {
    if (token.access_token.empty() || token.refresh_token.empty() || token.expires_in <= 0) {
        return outcome_error(Error::config("Codex token response is missing required fields"));
    }
    auto account_id = account_id_from_token(token.access_token);
    if (!account_id) return outcome_error(std::move(account_id).error());
    return Credentials{
        .access_token = std::move(token.access_token),
        .refresh_token = std::move(token.refresh_token),
        .expires_at = unix_milliseconds() + token.expires_in * 1000,
        .account_id = *std::move(account_id),
    };
}

Task<TokenResponse, Error> exchange_code(std::string_view base_url, const DeviceToken &device) {
    http::Client client;
    std::vector<http::QueryParam> fields;
    fields.push_back({.name = "grant_type", .value = "authorization_code"});
    fields.push_back({.name = "client_id", .value = std::string(k_client_id)});
    fields.push_back({.name = "code", .value = device.authorization_code});
    fields.push_back({.name = "code_verifier", .value = device.code_verifier});
    fields.push_back({.name = "redirect_uri", .value = std::string(k_device_redirect_uri)});
    auto request = client.on().post(std::string(base_url) + "/oauth/token");
    request.form(std::move(fields));
    auto sent = co_await std::move(request).send();
    if (!sent) co_await fail(Error::http(std::move(sent).error()));
    if (!sent->ok()) co_await fail(Error::http_status(sent->status, {}, sent->text_copy(), {}));
    co_return co_await lighter::or_fail(parse_json<TokenResponse>(sent->text(), "invalid Codex token response"));
}

Task<TokenResponse, Error> refresh_token(std::string_view base_url, std::string_view refresh) {
    http::Client client;
    std::vector<http::QueryParam> fields;
    fields.push_back({.name = "grant_type", .value = "refresh_token"});
    fields.push_back({.name = "refresh_token", .value = std::string(refresh)});
    fields.push_back({.name = "client_id", .value = std::string(k_client_id)});
    auto request = client.on().post(std::string(base_url) + "/oauth/token");
    request.form(std::move(fields));
    auto sent = co_await std::move(request).send();
    if (!sent) co_await fail(Error::http(std::move(sent).error()));
    if (!sent->ok()) co_await fail(Error::http_status(sent->status, {}, sent->text_copy(), {}));
    co_return co_await lighter::or_fail(parse_json<TokenResponse>(sent->text(), "invalid Codex refresh response"));
}

std::optional<u64> interval_seconds(const DeviceStart &start) {
    if (const auto *number = std::get_if<u64>(&start.interval)) return *number;
    const auto &text = std::get<std::string>(start.interval);
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

Task<provider::ResolvedAuth, Error> AuthState::resolve() {
    if (credentials.expires_at <= unix_milliseconds() + k_refresh_margin.count() * 60 * 1000) {
        auto token = co_await refresh_token(auth_base_url, credentials.refresh_token).or_fail();
        credentials = co_await lighter::or_fail(credentials_from_token(std::move(token)));
        co_await lighter::or_fail(write_auth_file(path, credentials));
    }
    co_return provider::ResolvedAuth{
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

std::filesystem::path default_auth_file() {
    if (const char *override_path = std::getenv("LIMINAL_AUTH_FILE"); override_path && *override_path) {
        return override_path;
    }
#ifdef _WIN32
    if (const char *app_data = std::getenv("APPDATA"); app_data && *app_data) {
        return std::filesystem::path(app_data) / "liminal" / "auth.json";
    }
#else
    if (const char *config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home) {
        return std::filesystem::path(config_home) / "liminal" / "auth.json";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "liminal" / "auth.json";
    }
#endif
    return std::filesystem::path("auth.json");
}

Result<std::optional<provider::AuthResolver>> load_auth(const std::filesystem::path &path) {
    auto file = read_auth_file(path);
    if (!file) return outcome_error(std::move(file).error());
    if (!file->codex) return std::optional<provider::AuthResolver>{};
    if (file->codex->type != "oauth" || file->codex->access_token.empty() || file->codex->refresh_token.empty() ||
        file->codex->account_id.empty()) {
        return outcome_error(Error::config("auth file '" + path.string() + "' has invalid Codex credentials"));
    }
    auto state = std::make_shared<AuthState>(AuthState{
        .credentials = *std::move(file->codex),
        .path = path,
        .auth_base_url = auth_base_url(),
    });
    provider::AuthResolver resolver = [state]() { return state->resolve(); };
    return std::optional<provider::AuthResolver>(std::move(resolver));
}

Task<void, Error> login_device(std::filesystem::path path, DeviceCodeNotice notice) {
    const auto base_url = auth_base_url();
    http::Client client;
    auto body = json::to_string(DeviceStartRequest{.client_id = std::string(k_client_id)});
    if (!body) co_await fail(Error::json(std::move(body).error(), "Codex device login request"));
    auto started = co_await client.on().post(base_url + "/api/accounts/deviceauth/usercode").json_text(*body).send();
    if (!started) co_await fail(Error::http(std::move(started).error()));
    if (!started->ok()) co_await fail(Error::http_status(started->status, {}, started->text_copy(), {}));
    auto device = co_await lighter::or_fail(parse_json<DeviceStart>(started->text(), "invalid Codex device login response"));
    auto parsed_interval = interval_seconds(device);
    if (device.device_auth_id.empty() || device.user_code.empty() || !parsed_interval) {
        co_await fail(Error::config("Codex device login response is missing required fields"));
    }
    auto interval = *parsed_interval;

    const auto verification_url = base_url + "/codex/device";
    notice(verification_url, device.user_code);
    const auto deadline = std::chrono::steady_clock::now() + k_login_timeout;
    DeviceToken device_token;
    while (std::chrono::steady_clock::now() < deadline) {
        co_await lighter::sleep(std::chrono::seconds(interval));
        auto poll_body = json::to_string(DeviceTokenRequest{.device_auth_id = device.device_auth_id, .user_code = device.user_code});
        if (!poll_body) co_await fail(Error::json(std::move(poll_body).error(), "Codex device token request"));
        auto polled = co_await client.on().post(base_url + "/api/accounts/deviceauth/token").json_text(*poll_body).send();
        if (!polled) co_await fail(Error::http(std::move(polled).error()));
        if (polled->ok()) {
            device_token = co_await lighter::or_fail(parse_json<DeviceToken>(polled->text(), "invalid Codex device token response"));
            break;
        }
        const auto error_body = polled->text();
        if (error_body.find("slow_down") != std::string_view::npos) {
            interval += 5;
            continue;
        }
        if (polled->status == 403 || polled->status == 404 ||
            error_body.find("deviceauth_authorization_pending") != std::string_view::npos) {
            continue;
        }
        co_await fail(Error::http_status(polled->status, {}, polled->text_copy(), {}));
    }
    if (device_token.authorization_code.empty() || device_token.code_verifier.empty()) {
        co_await fail(Error::config("Codex device login timed out"));
    }

    auto token = co_await exchange_code(base_url, device_token).or_fail();
    auto credentials = co_await lighter::or_fail(credentials_from_token(std::move(token)));
    co_await lighter::or_fail(write_auth_file(path, std::move(credentials)));
}

} // namespace liminal::codex
