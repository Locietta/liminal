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

#include <liminal/provider/detail/codex_auth_flow.h>

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

struct AuthFile {
    std::optional<detail::Credentials> codex;
};

struct DeviceStartResponse {
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

struct AuthState {
    detail::Credentials credentials;
    std::filesystem::path path;
    std::string auth_base_url;

    Task<provider::ResolvedAuth, Error> resolve();
};

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
        return outcome_error(Error::config("cannot inspect auth file '" + path.string() + "': " + exists_error.message(), ErrorCode::IO));
    }
    if (!exists) {
        return outcome_error(Error::config("auth file was not found: '" + path.string() + "'", ErrorCode::NOT_FOUND));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return outcome_error(Error::config("cannot open auth file '" + path.string() + "'", ErrorCode::IO));
    }
    std::string text(std::istreambuf_iterator<char>(input), {});
    auto file = parse_json<AuthFile>(text, "invalid auth file '" + path.string() + "'");
    if (!file) {
        auto error = std::move(file).error();
        error.code = ErrorCode::AUTH_INVALID;
        return outcome_error(std::move(error));
    }
    return file;
}

Result<void> write_auth_file(const std::filesystem::path &path, detail::Credentials credentials) {
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

Task<detail::TokenResponse, Error> exchange_code(std::string_view base_url, const detail::DeviceToken &device) {
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
    co_return co_await lighter::or_fail(parse_json<detail::TokenResponse>(sent->text(), "invalid Codex token response"));
}

Task<detail::TokenResponse, Error> refresh_token(std::string_view base_url, std::string_view refresh) {
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
    co_return co_await lighter::or_fail(parse_json<detail::TokenResponse>(sent->text(), "invalid Codex refresh response"));
}

std::optional<u64> interval_seconds(const DeviceStartResponse &start) {
    if (const auto *number = std::get_if<u64>(&start.interval)) return *number;
    const auto &text = std::get<std::string>(start.interval);
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

Task<detail::DeviceStart, Error> start_device(std::string_view base_url) {
    http::Client client;
    auto body = json::to_string(DeviceStartRequest{.client_id = std::string(k_client_id)});
    if (!body) co_await fail(Error::json(std::move(body).error(), "Codex device login request"));
    auto sent = co_await client.on().post(std::string(base_url) + "/api/accounts/deviceauth/usercode").json_text(*body).send();
    if (!sent) co_await fail(Error::http(std::move(sent).error()));
    if (!sent->ok()) co_await fail(Error::http_status(sent->status, {}, sent->text_copy(), {}));

    auto response = co_await lighter::or_fail(parse_json<DeviceStartResponse>(sent->text(), "invalid Codex device login response"));
    auto interval = interval_seconds(response);
    if (!interval) co_await fail(Error::config("Codex device login response is missing required fields"));
    co_return detail::DeviceStart{
        .verification_url = std::string(base_url) + "/codex/device",
        .device_auth_id = std::move(response.device_auth_id),
        .user_code = std::move(response.user_code),
        .interval = std::chrono::seconds(*interval),
    };
}

Task<detail::DeviceToken, Error> poll_device(std::string_view base_url, const detail::DeviceStart &device) {
    http::Client client;
    auto body = json::to_string(DeviceTokenRequest{.device_auth_id = device.device_auth_id, .user_code = device.user_code});
    if (!body) co_await fail(Error::json(std::move(body).error(), "Codex device token request"));
    auto sent = co_await client.on().post(std::string(base_url) + "/api/accounts/deviceauth/token").json_text(*body).send();
    if (!sent) co_await fail(Error::http(std::move(sent).error()));
    if (!sent->ok()) co_await fail(Error::http_status(sent->status, {}, sent->text_copy(), {}));
    co_return co_await lighter::or_fail(parse_json<detail::DeviceToken>(sent->text(), "invalid Codex device token response"));
}

Task<provider::ResolvedAuth, Error> AuthState::resolve() {
    detail::RefreshAttempts attempts{
        .refresh = [base_url = auth_base_url](const std::string &refresh) { return refresh_token(base_url, refresh); },
        .save = [path = path](const detail::Credentials &updated) { return write_auth_file(path, updated); },
        .now_unix_milliseconds = [] { return unix_milliseconds(); },
    };
    co_return co_await detail::resolve_auth(attempts, credentials, k_refresh_margin).or_fail();
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

Result<provider::AuthResolver> load_auth(const std::filesystem::path &path) {
    auto file = read_auth_file(path);
    if (!file) {
        auto error = std::move(file).error();
        if (error.code == ErrorCode::NOT_FOUND) {
            error.code = ErrorCode::AUTH_NOT_CONFIGURED;
        }
        return outcome_error(std::move(error));
    }
    if (!file->codex) {
        return outcome_error(Error::config("auth file '" + path.string() + "' has no Codex credentials", ErrorCode::AUTH_NOT_CONFIGURED));
    }
    if (file->codex->type != "oauth" || file->codex->access_token.empty() || file->codex->refresh_token.empty() ||
        file->codex->account_id.empty()) {
        return outcome_error(Error::config("auth file '" + path.string() + "' has invalid Codex credentials", ErrorCode::AUTH_INVALID));
    }
    auto state = std::make_shared<AuthState>(AuthState{
        .credentials = *std::move(file->codex),
        .path = path,
        .auth_base_url = auth_base_url(),
    });
    provider::AuthResolver resolver = [state]() { return state->resolve(); };
    return resolver;
}

Task<void, Error> login_device(std::filesystem::path path, DeviceCodeNotice notice) {
    const auto base_url = auth_base_url();
    detail::DeviceLoginAttempts attempts{
        .start = [base_url] { return start_device(base_url); },
        .poll = [base_url](const detail::DeviceStart &device) { return poll_device(base_url, device); },
        .exchange = [base_url](const detail::DeviceToken &device) { return exchange_code(base_url, device); },
        .sleep = [](std::chrono::seconds delay) { return lighter::sleep(delay); },
        .save = [path = std::move(path)](const detail::Credentials &credentials) { return write_auth_file(path, credentials); },
        .now = [] { return std::chrono::steady_clock::now(); },
        .now_unix_milliseconds = [] { return unix_milliseconds(); },
    };
    co_return co_await detail::login_device(attempts, std::move(notice), k_login_timeout).or_fail();
}

} // namespace liminal::codex
