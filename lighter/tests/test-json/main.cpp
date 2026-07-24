#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lighter/codec/json/json.h>
#include <lighter/http/json.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

enum struct Role {
    SYSTEM,
    USER,
    ASSISTANT,
};

struct Message {
    Role role = Role::USER;
    std::string content;
    std::optional<std::string> name;
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    f64 temperature = 1.0;
    std::map<std::string, std::string> metadata;
};

// A non-aggregate with a constructor and a private member: only decodable
// through the C++26 static reflection backend, not glaze's aggregate fallback.
struct Counter {
    Counter() = default;

    explicit Counter(i64 start) : count_(start) {}

    i64 value() const { return count_; }

private:
    i64 count_ = 0;
};

void test_round_trip() {
    ChatRequest request{
        .model = "fable-5",
        .messages = {{.role = Role::SYSTEM, .content = "be brief"}, {.role = Role::USER, .content = "hi \"there\"\n", .name = "loia"}},
        .temperature = 0.5,
        .metadata = {{"session", "abc123"}},
    };

    auto encoded = codec::json::to_string(request);
    require(static_cast<bool>(encoded), "encode failed");

    auto decoded = codec::json::parse<ChatRequest>(*encoded);
    require(static_cast<bool>(decoded), decoded ? std::string() : std::string(decoded.error().message()));
    require(decoded->model == request.model, "model mismatch after round trip");
    require(decoded->messages.size() == 2, "message count mismatch after round trip");
    require(decoded->messages[0].role == Role::SYSTEM, "enum value mismatch after round trip");
    require(decoded->messages[1].content == request.messages[1].content, "string escaping mismatch after round trip");
    require(decoded->messages[1].name == request.messages[1].name, "optional field mismatch after round trip");
    require(!decoded->messages[0].name.has_value(), "absent optional decoded as present");
    require(decoded->temperature == request.temperature, "float mismatch after round trip");
    require(decoded->metadata == request.metadata, "map mismatch after round trip");
}

void test_reflected_enum_names() {
    // With the P2996 backend glaze serializes enums by name without glz::meta.
    auto encoded = codec::json::to_string(Role::ASSISTANT);
    require(static_cast<bool>(encoded), "enum encode failed");
    require(*encoded == "\"ASSISTANT\"", "enum did not serialize by reflected name, got: " + *encoded);
}

void test_non_aggregate() {
    Counter counter{42};
    auto encoded = codec::json::to_string(counter);
    require(static_cast<bool>(encoded), "non-aggregate encode failed");

    auto decoded = codec::json::parse<Counter>(*encoded);
    require(static_cast<bool>(decoded), "non-aggregate decode failed");
    require(decoded->value() == 42, "private member mismatch after round trip");
}

void test_decode_errors() {
    auto truncated = codec::json::parse<ChatRequest>(R"({"model": "fable-5", "messages": [)");
    require(!truncated, "truncated json unexpectedly decoded");
    require(!truncated.error().message().empty(), "decode error carries no message");

    auto mistyped = codec::json::parse<ChatRequest>(R"({"model": 17})");
    require(!mistyped, "mistyped json unexpectedly decoded");

    // Not null-terminated at the cutoff point: verifies the null_terminated=false opts.
    const std::string backing = R"({"model": "x", "messages": []}garbage)";
    auto windowed = codec::json::parse<ChatRequest>(std::string_view(backing).substr(0, 30));
    require(static_cast<bool>(windowed), "windowed non-null-terminated parse failed");
    require(windowed->model == "x", "windowed parse mismatch");
}

void test_http_response_json() {
    const std::string_view payload = R"({"role":"ASSISTANT","content":"ok"})";
    http::Response response{.status = 200};
    response.body.assign(reinterpret_cast<const byte *>(payload.data()), reinterpret_cast<const byte *>(payload.data() + payload.size()));

    auto message = response.json<Message>();
    require(static_cast<bool>(message), message ? std::string() : message.error().message());
    require(message->role == Role::ASSISTANT, "response json role mismatch");
    require(message->content == "ok", "response json content mismatch");

    response.body.assign(3, byte{'!'});
    auto bad = response.json<Message>();
    require(!bad, "malformed response body unexpectedly decoded");
    require(bad.error().kind == http::ErrorKind::JSON_DECODE, "wrong error kind for json decode failure");
}

void test_prettify_minify() {
    const std::string compact = R"({"a":[1,2],"b":"c"})";
    auto pretty = codec::json::prettify(compact);
    require(pretty.find('\n') != std::string::npos, "prettify produced no line breaks");
    require(codec::json::minify(pretty) == compact, "minify(prettify(x)) != x");
}

} // namespace

int main() {
    test_round_trip();
    test_reflected_enum_names();
    test_non_aggregate();
    test_decode_errors();
    test_http_response_json();
    test_prettify_minify();
    return 0;
}
