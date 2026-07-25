#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/http/http.h>
#include <lighter/http/text.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

Task<> write_all(Tcp &connection, std::string_view text) {
    auto written = co_await connection.write(std::span(text.data(), text.size()));
    require(static_cast<bool>(written), "test server failed to write");
}

Task<> accept_one(Tcp::Acceptor &listener, Tcp &out) {
    auto accepted = co_await listener.accept();
    require(static_cast<bool>(accepted), "test server failed to accept");
    listener = {};
    out = std::move(*accepted);
    auto request = co_await out.read();
    require(static_cast<bool>(request), "test server failed to read the request");
}

// --- scenario 1: multi-byte sequences split across TCP writes ---------------

// "中文" is E4 B8 AD, E6 96 87; the emoji is F0 9F 98 80. Every write below
// ends mid-sequence, so raw read_chunk() views would tear characters.
Task<> serve_torn_utf8(Tcp::Acceptor listener) {
    Tcp connection;
    co_await accept_one(listener, connection);

    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain; charset=utf-8\r\n"
                                   "Connection: close\r\n"
                                   "\r\n");

    co_await write_all(connection, "ok \xE4");
    co_await sleep(std::chrono::milliseconds(10));
    co_await write_all(connection, "\xB8\xAD\xE6\x96");
    co_await sleep(std::chrono::milliseconds(10));
    co_await write_all(connection, "\x87 \xF0\x9F");
    co_await sleep(std::chrono::milliseconds(10));
    co_await write_all(connection, "\x98\x80 end");
    connection = {};
}

Task<> consume_torn_utf8(i32 port) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/text").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());

    http::TextStream text(std::move(*stream_result));
    require(text.http_response().status == 200, "status mismatch");

    std::string assembled;
    while (true) {
        auto piece = co_await text.next();
        require(static_cast<bool>(piece), piece ? std::string() : piece.error().message());
        if (piece->empty()) {
            break;
        }
        // the guarantee under test: every piece is well-formed on its own
        require(encoding::utf8::is_valid(*piece), "TextStream piece must be complete UTF-8");
        assembled += *piece;
    }

    require(assembled == "ok 中文 \xF0\x9F\x98\x80 end", "reassembled text mismatch");
}

// --- scenario 2: connection drops mid-sequence -> replacement, not garbage --

Task<> serve_truncated_utf8(Tcp::Acceptor listener) {
    Tcp connection;
    co_await accept_one(listener, connection);
    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain; charset=utf-8\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "cut \xF0\x9F"); // emoji forever incomplete
    connection = {};
}

Task<> consume_truncated_utf8(i32 port) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/text").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());

    http::TextStream text(std::move(*stream_result));
    std::string assembled;
    while (true) {
        auto piece = co_await text.next();
        require(static_cast<bool>(piece), piece ? std::string() : piece.error().message());
        if (piece->empty()) {
            break;
        }
        assembled += *piece;
    }

    require(assembled == "cut \xEF\xBF\xBD", "truncated tail must become U+FFFD");
}

// --- harness ----------------------------------------------------------------

i32 listen_port(Tcp::Acceptor &listener) {
    auto port = Tcp::local_port(listener);
    require(static_cast<bool>(port), "failed to query listener port");
    return *port;
}

Tcp::Acceptor make_listener(EventLoop &loop) {
    auto listener = Tcp::listen("127.0.0.1", 0, {}, loop);
    require(static_cast<bool>(listener), "failed to create test listener");
    return std::move(*listener);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    EventLoop loop;

    std::printf("scenario 1\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        auto server = serve_torn_utf8(std::move(listener));
        auto client = consume_torn_utf8(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        server.value();
        client.value();
    }

    std::printf("scenario 2\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        auto server = serve_truncated_utf8(std::move(listener));
        auto client = consume_truncated_utf8(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        server.value();
        client.value();
    }

    return 0;
}
