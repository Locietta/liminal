#include <chrono>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <lighter/async/async.h>
#include <lighter/codec/json/json.h>
#include <lighter/http/http.h>
#include <lighter/http/inflight_request.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;
using namespace std::chrono_literals;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

Task<> write_all(Tcp &connection, std::string_view text) {
    auto written = co_await connection.write(std::span(text.data(), text.size()));
    require(static_cast<bool>(written), "test server failed to write");
}

/// Accept a connection into `out` and swallow the client's request bytes.
/// Closes the listener: the coroutine frame outlives loop.run(), so a live
/// acceptor would keep the loop from ever returning.
Task<> accept_one(Tcp::Acceptor &listener, Tcp &out) {
    auto accepted = co_await listener.accept();
    require(static_cast<bool>(accepted), "test server failed to accept");
    listener = {};
    out = std::move(*accepted);
    auto request = co_await out.read();
    require(static_cast<bool>(request), "test server failed to read the request");
}

/// EOF or a read error after the peer vanished both count as "disconnected".
Task<bool> peer_disconnected(Tcp &connection) {
    auto data = co_await connection.read();
    co_return !data || data->empty();
}

// --- scenario 1: end-to-end SSE stream, headers early, clean close ----------

Task<> serve_sse_stream(Tcp::Acceptor listener) {
    Tcp connection;
    co_await accept_one(listener, connection);

    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/event-stream\r\n"
                                   "Cache-Control: no-cache\r\n"
                                   "Connection: close\r\n"
                                   "\r\n");

    // deliberately awkward chunk boundaries: mid-line, mid-event
    co_await write_all(connection, ": warm-up\n\nevent: delta\ndata: {\"tex");
    co_await yield();
    co_await write_all(connection, "t\":\"Hel\"}\n\nevent: delta\ndata");
    co_await yield();
    co_await write_all(connection, ": {\"text\":\"lo\"}\n\nevent: done\ndata: {}\n\n");
    connection = {};
}

Task<> consume_sse_stream(i32 port) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/v1/stream").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());
    auto streamed = std::move(*stream_result);

    // headers resolved before any body was consumed
    require(streamed.ok() && streamed.status == 200, "stream() must resolve with the status");
    require(streamed.header_value("content-type") == std::string_view("text/event-stream"), "stream() must expose response headers");

    struct Delta {
        std::optional<std::string> text;
    };

    http::sse::EventStream events(std::move(streamed));
    std::string text;
    std::vector<std::string> kinds;
    while (true) {
        auto next_result = co_await events.next();
        require(static_cast<bool>(next_result), next_result ? std::string() : next_result.error().message());
        auto event = std::move(*next_result);
        if (!event) {
            break;
        }
        kinds.push_back(event->event);
        if (event->event == "delta") {
            auto delta = codec::json::parse<Delta>(event->data);
            require(static_cast<bool>(delta), "delta payload failed to decode");
            if (delta->text) {
                text += *delta->text;
            }
        }
    }

    require(kinds == std::vector<std::string>({"delta", "delta", "done"}), "unexpected event sequence");
    require(text == "Hello", "streamed deltas did not reassemble");
}

// --- scenario 2: non-2xx branch - check status, drop, transfer aborts -------

Task<> serve_rate_limited(Tcp::Acceptor listener, bool *observed_disconnect) {
    Tcp connection;
    co_await accept_one(listener, connection);

    // Content-Length promises more than is sent, so the transfer is
    // mid-flight when the client drops it.
    co_await write_all(connection, "HTTP/1.1 429 Too Many Requests\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Retry-After: 7\r\n"
                                   "Content-Length: 4096\r\n"
                                   "\r\n"
                                   "{\"error\":");

    *observed_disconnect = co_await peer_disconnected(connection);
    connection = {};
}

Task<> consume_rate_limited(i32 port) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/v1/stream").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());
    auto streamed = std::move(*stream_result);

    require(streamed.status == 429, "expected the 429 status at header time");
    require(streamed.header_value("retry-after") == std::string_view("7"), "retry-after header missing");
    // just drop it - no drain, no close(); destruction aborts the transfer
}

// --- scenario 3: user interrupt - cancellation token mid-stream -------------

Task<> serve_stalling_stream(Tcp::Acceptor listener, bool *observed_disconnect) {
    Tcp connection;
    co_await accept_one(listener, connection);

    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/event-stream\r\n"
                                   "\r\n"
                                   "event: delta\ndata: {\"text\":\"thinking...\"}\n\n");

    // then stall, as if generation hung; the client interrupt must not wait us out
    *observed_disconnect = co_await peer_disconnected(connection);
    connection = {};
}

Task<usize, http::Error> consume_until_cancelled(http::sse::EventStream events, usize *events_seen, Event &first_event) {
    while (true) {
        auto next_result = co_await events.next();
        require(static_cast<bool>(next_result), next_result ? std::string() : next_result.error().message());
        auto event = std::move(*next_result);
        if (!event) {
            break;
        }
        *events_seen += 1;
        if (*events_seen == 1) first_event.set();
    }
    co_return *events_seen;
}

Task<> consume_with_interrupt(i32 port, CancellationToken token, usize *events_seen, Event &first_event) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/v1/stream").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());
    auto streamed = std::move(*stream_result);
    require(streamed.status == 200, "stalling stream must resolve headers");

    http::sse::EventStream events(std::move(streamed));
    auto outcome = co_await with_token(consume_until_cancelled(std::move(events), events_seen, first_event), token);

    require(outcome.is_cancelled(), "interrupt must surface as cancellation, not error/value");
    // unwinding consume_until_cancelled destroyed the EventStream -> transfer aborted
}

Task<> fire_interrupt(CancellationSource &source, Event &first_event) {
    co_await first_event.wait();
    source.cancel();
}

// --- scenario 4: per-event idle timeout via WhenAny race --------------------

Task<> serve_one_then_stall(Tcp::Acceptor listener, bool *observed_disconnect) {
    Tcp connection;
    co_await accept_one(listener, connection);
    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/event-stream\r\n"
                                   "\r\n"
                                   "data: {\"n\":1}\n\n");
    *observed_disconnect = co_await peer_disconnected(connection);
    connection = {};
}

Task<> consume_with_idle_timeout(i32 port) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/v1/stream").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());
    auto streamed = std::move(*stream_result);
    http::sse::EventStream events(std::move(streamed));

    // first event arrives within the idle window
    {
        auto raced = co_await WhenAny(events.next(), sleep(1000ms));
        require(raced.has_value() && raced->index() == 0, "first event must beat the idle timeout");
        require(std::get<0>(*raced).has_value(), "first event missing");
    }

    // then the stream stalls; the timeout must win the race
    {
        auto raced = co_await WhenAny(events.next(), sleep(50ms));
        require(raced.has_value() && raced->index() == 1, "idle timeout must win against a stalled stream");
    }
    // dropping `events` aborts the stalled transfer
}

// --- scenario 5: backpressure on a large non-SSE body -----------------------

Task<> serve_large_body(Tcp::Acceptor listener, std::string_view payload) {
    Tcp connection;
    co_await accept_one(listener, connection);
    co_await write_all(connection, "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/octet-stream\r\n"
                                   "Content-Length: " +
                                       std::to_string(payload.size()) +
                                       "\r\n"
                                       "\r\n");
    // burst everything; the consumer stalls below, forcing the pause path
    for (usize off = 0; off < payload.size(); off += 64 * 1024) {
        co_await write_all(connection, payload.substr(off, 64 * 1024));
    }
    connection = {};
}

Task<> consume_large_body(i32 port, std::string_view payload) {
    http::Client client;
    auto stream_result = co_await client.on().get("http://127.0.0.1:" + std::to_string(port) + "/big").stream();
    require(static_cast<bool>(stream_result), stream_result ? std::string() : stream_result.error().message());
    auto streamed = std::move(*stream_result);
    require(streamed.status == 200, "large body status mismatch");

    // Give delivery callbacks an iteration before pulling. The precise
    // backpressure threshold is verified directly below.
    co_await yield();

    std::string received;
    received.reserve(payload.size());
    while (true) {
        auto chunk_result = co_await streamed.read_chunk();
        require(static_cast<bool>(chunk_result), chunk_result ? std::string() : chunk_result.error().message());
        auto chunk = *chunk_result;
        if (chunk.empty()) {
            break;
        }
        received.append(chunk.data(), chunk.size());
        streamed.consume(chunk.size());
    }

    require(received.size() == payload.size(), "large body size mismatch: got " + std::to_string(received.size()));
    require(received == payload, "large body content mismatch");
}

void check_backpressure_threshold(EventLoop &loop) {
    http::Client client;
    http::detail::InflightRequest request(client.on(loop).get("http://127.0.0.1/unused"));
    request.enable_streaming();
    auto &conduit = *request.conduit;
    conduit.buffer.assign(http::detail::StreamConduit::k_high_water, 'x');

    char byte = 'y';
    const auto accepted = http::detail::InflightRequest::on_write(&byte, 1, 1, &request);
    require(accepted == CURL_WRITEFUNC_PAUSE && conduit.paused && conduit.buffer.size() == http::detail::StreamConduit::k_high_water,
            "streaming backpressure did not pause before accepting bytes above the high-water mark");
}

// --- scenario 6: proxy CONNECT headers are not origin response headers -----

Task<> serve_connect_only_proxy(Tcp::Acceptor listener) {
    Tcp connection;
    co_await accept_one(listener, connection);
    co_await write_all(connection, "HTTP/1.1 200 Connection established\r\n"
                                   "Proxy-Agent: test\r\n"
                                   "\r\n");
    connection = {};
}

Task<> reject_connect_headers_as_response(i32 port) {
    http::Client client;
    auto stream_result =
        co_await client.on().get("https://example.invalid/stream").proxy("http://127.0.0.1:" + std::to_string(port)).stream();
    require(!stream_result, "proxy CONNECT headers must not resolve a streaming origin response");
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
    check_backpressure_threshold(loop);

    // scenario 1
    std::printf("scenario 1\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        auto server = serve_sse_stream(std::move(listener));
        auto client = consume_sse_stream(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
    }

    // scenario 2
    std::printf("scenario 2\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        bool disconnected = false;
        auto server = serve_rate_limited(std::move(listener), &disconnected);
        auto client = consume_rate_limited(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
        require(disconnected, "dropping the 429 stream must abort the transfer");
    }

    // scenario 3
    std::printf("scenario 3\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        bool disconnected = false;
        usize events_seen = 0;
        CancellationSource interrupt;
        Event first_event;
        auto server = serve_stalling_stream(std::move(listener), &disconnected);
        auto client = consume_with_interrupt(port, interrupt.token(), &events_seen, first_event);
        auto trigger = fire_interrupt(interrupt, first_event);
        loop.schedule(server);
        loop.schedule(client);
        loop.schedule(trigger);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
        std::ignore = trigger.value();
        require(events_seen == 1, "one event should arrive before the interrupt");
        require(disconnected, "cancelling the consumer must abort the transfer");
    }

    // scenario 4
    std::printf("scenario 4\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        bool disconnected = false;
        auto server = serve_one_then_stall(std::move(listener), &disconnected);
        auto client = consume_with_idle_timeout(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
        require(disconnected, "dropping after the timeout must abort the transfer");
    }

    // scenario 5
    std::printf("scenario 5\n");
    {
        std::string payload;
        payload.reserve(1024 * 1024);
        for (usize i = 0; payload.size() < 1024 * 1024; ++i) {
            payload += "chunk-" + std::to_string(i) + "-abcdefghijklmnopqrstuvwxyz-";
        }
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        auto server = serve_large_body(std::move(listener), payload);
        auto client = consume_large_body(port, payload);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
    }

    // scenario 6
    std::printf("scenario 6\n");
    {
        auto listener = make_listener(loop);
        auto port = listen_port(listener);
        auto server = serve_connect_only_proxy(std::move(listener));
        auto client = reject_connect_headers_as_response(port);
        loop.schedule(server);
        loop.schedule(client);
        loop.run();
        std::ignore = server.value();
        std::ignore = client.value();
    }

    std::printf("all scenarios passed\n");
    return 0;
}
