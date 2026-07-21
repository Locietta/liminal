#include <lighter/async/io/loop.h>
#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/task.h>
#include <lighter/http/http.h>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

lighter::Task<> serve_once(lighter::Tcp::Acceptor listener) {
    auto accepted = co_await listener.accept();
    require(static_cast<bool>(accepted), "failed to accept the HTTP test connection");
    listener = {};

    auto connection = std::move(*accepted);
    auto request = co_await connection.read();
    require(static_cast<bool>(request), "failed to read the HTTP test request");
    require(request->starts_with("GET /resource?name=lighter HTTP/1.1\r\n"), "HTTP client sent an unexpected request target");

    constexpr std::string_view response = "HTTP/1.1 200 OK\r\n"
                                          "Content-Type: text/plain\r\n"
                                          "X-Test-Server: lighter\r\n"
                                          "Content-Length: 12\r\n"
                                          "Connection: close\r\n"
                                          "\r\n"
                                          "hello, http!";
    auto written = co_await connection.write(std::span(response.data(), response.size()));
    require(static_cast<bool>(written), "failed to write the HTTP test response");
    connection = {};
}

lighter::Task<> make_request(lighter::i32 port) {
    lighter::http::Client client;
    auto request = client.on().get("http://127.0.0.1:" + std::to_string(port) + "/resource");
    request.query("name", "lighter");

    auto result = co_await std::move(request).send();
    require(static_cast<bool>(result), result ? std::string() : result.error().message());

    const auto &response = *result;
    require(response.status == 200, "HTTP client returned an unexpected status");
    require(response.text() == "hello, http!", "HTTP client returned an unexpected body");
    require(response.header_value("x-test-server") == std::string_view("lighter"),
            "HTTP client did not parse response headers case-insensitively");
}

} // namespace

int main() {
    lighter::EventLoop loop;
    auto listener = lighter::Tcp::listen("127.0.0.1", 0, {}, loop);
    require(static_cast<bool>(listener), "failed to create the HTTP test listener");

    auto port = lighter::Tcp::local_port(*listener);
    require(static_cast<bool>(port), "failed to query the HTTP test listener port");

    auto server = serve_once(std::move(*listener));
    auto client = make_request(*port);
    loop.schedule(server);
    loop.schedule(client);
    loop.run();

    server.value();
    client.value();
    return 0;
}
