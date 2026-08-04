#include <cstdio>
#include <exception>
#include <expected>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glaze/json.hpp>

#include <lighter/codec/json/json.h>
#include <lighter/types.hpp>

#include "liminal/tui/headless.h"

namespace liminal::dev_mcp {

namespace json = lighter::codec::json;
using namespace lighter::types;

namespace {

constexpr usize k_max_sessions = 32;

struct RpcRequest {
    std::string jsonrpc;
    glz::generic id;
    std::string method;
    glz::generic params;
};

struct CallToolInput {
    glz::generic _meta;
    std::string name;
    glz::generic arguments;
};

struct CreateInput {
    std::string driver = "tui.headless";
    i32 columns = 80;
    i32 rows = 24;
    i64 now_ms = 0;
};

struct ApplyInput {
    std::string session_id;
    std::vector<tui::HeadlessAction> actions;
};

struct SessionInput {
    std::string session_id;
};

glz::generic object() { return glz::generic::object_t{}; }

glz::generic array() { return glz::generic::array_t{}; }

glz::generic parse_literal(std::string_view text) {
    auto value = json::parse<glz::generic>(text);
    if (!value) return object();
    return *std::move(value);
}

template <typename T>
std::expected<T, std::string> decode(const glz::generic &value) {
    auto encoded = json::to_string(value);
    if (!encoded) return std::unexpected(encoded.error().detail);
    auto parsed = json::parse<T>(*encoded);
    if (!parsed) return std::unexpected(parsed.error().detail);
    return *std::move(parsed);
}

glz::generic rpc_result(const glz::generic &id, glz::generic result) {
    glz::generic response = object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

glz::generic rpc_error(const glz::generic &id, i32 code, std::string message) {
    glz::generic error = object();
    error["code"] = code;
    error["message"] = std::move(message);
    glz::generic response = object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = std::move(error);
    return response;
}

glz::generic tool_result(glz::generic structured, bool is_error = false) {
    auto text = json::to_string(structured);
    glz::generic content = object();
    content["type"] = "text";
    content["text"] = text ? *std::move(text) : std::string("cannot encode tool result");
    glz::generic contents = array();
    contents.get_array().push_back(std::move(content));

    glz::generic result = object();
    result["resultType"] = "complete";
    result["content"] = std::move(contents);
    result["structuredContent"] = std::move(structured);
    result["isError"] = is_error;
    return result;
}

glz::generic execution_error(std::string message) {
    glz::generic structured = object();
    structured["error"] = std::move(message);
    return tool_result(std::move(structured), true);
}

glz::generic snapshot_value(const tui::HeadlessSession &session) {
    auto encoded = json::to_string(session.inspect());
    if (!encoded) {
        glz::generic failed = object();
        failed["serialization_error"] = encoded.error().detail;
        return failed;
    }
    auto value = json::parse<glz::generic>(*encoded);
    if (!value) {
        glz::generic failed = object();
        failed["serialization_error"] = value.error().detail;
        return failed;
    }
    return *std::move(value);
}

glz::generic tools() {
    return parse_literal(R"json([
      {
        "name":"session_create",
        "title":"Create headless UI session",
        "description":"Create a bounded deterministic tui.headless session. It has no process, network, or workspace access.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"driver":{"type":"string","enum":["tui.headless"],"default":"tui.headless"},"columns":{"type":"integer","minimum":1,"maximum":500,"default":80},"rows":{"type":"integer","minimum":1,"maximum":200,"default":24},"now_ms":{"type":"integer","minimum":0,"default":0}}},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
      },
      {
        "name":"session_apply",
        "title":"Apply headless UI actions",
        "description":"Apply a batch of typed actions. Rendering coalesces until a flush action or 16ms of virtual time advances.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string"},"actions":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{"type":{"type":"string"},"text":{"type":"string"},"call_id":{"type":"string"},"name":{"type":"string"},"effort":{"type":["string","null"]},"columns":{"type":"integer"},"rows":{"type":"integer"},"amount":{"type":"integer"},"milliseconds":{"type":"integer"},"is_error":{"type":"boolean"}},"required":["type"],"additionalProperties":false}}},"required":["session_id","actions"]},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
      },
      {
        "name":"session_inspect",
        "title":"Inspect headless UI session",
        "description":"Return transcript, viewport, composer, visible cells, cache diagnostics, and emitted ANSI operations.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string"}},"required":["session_id"]},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
      },
      {
        "name":"session_close",
        "title":"Close headless UI session",
        "description":"Release a bounded headless session handle.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string"}},"required":["session_id"]},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
      }
    ])json");
}

struct Server {
    glz::generic call_tool(const CallToolInput &call) {
        if (call.name == "session_create") {
            auto input = decode<CreateInput>(call.arguments);
            if (!input) return execution_error(input.error());
            if (input->driver != "tui.headless") return execution_error("unsupported driver: " + input->driver);
            if (sessions.size() >= k_max_sessions) return execution_error("server session limit exceeded");
            if (input->columns < 1 || input->columns > 500 || input->rows < 1 || input->rows > 200 || input->now_ms < 0) {
                return execution_error("invalid terminal size or virtual time");
            }
            auto id = "session-" + std::to_string(next_id++);
            auto session = std::make_unique<tui::HeadlessSession>(input->columns, input->rows, input->now_ms);
            glz::generic structured = object();
            structured["session_id"] = id;
            structured["capabilities"] = parse_literal(
                R"json({"driver":"tui.headless","version":"1","actions":true,"virtual_clock":true,"snapshots":true,"process":false,"network":false,"workspace":false})json");
            structured["snapshot"] = snapshot_value(*session);
            sessions.emplace(id, std::move(session));
            return tool_result(std::move(structured));
        }

        auto get_session = [this](std::string_view id) -> tui::HeadlessSession * {
            const auto found = sessions.find(std::string(id));
            return found == sessions.end() ? nullptr : found->second.get();
        };

        if (call.name == "session_apply") {
            auto input = decode<ApplyInput>(call.arguments);
            if (!input) return execution_error(input.error());
            auto *session = get_session(input->session_id);
            if (!session) return execution_error("unknown session: " + input->session_id);
            if (auto applied = session->apply(input->actions); !applied) return execution_error(applied.error());
            glz::generic structured = object();
            structured["session_id"] = input->session_id;
            structured["snapshot"] = snapshot_value(*session);
            return tool_result(std::move(structured));
        }
        if (call.name == "session_inspect") {
            auto input = decode<SessionInput>(call.arguments);
            if (!input) return execution_error(input.error());
            auto *session = get_session(input->session_id);
            if (!session) return execution_error("unknown session: " + input->session_id);
            glz::generic structured = object();
            structured["session_id"] = input->session_id;
            structured["snapshot"] = snapshot_value(*session);
            return tool_result(std::move(structured));
        }
        if (call.name == "session_close") {
            auto input = decode<SessionInput>(call.arguments);
            if (!input) return execution_error(input.error());
            if (sessions.erase(input->session_id) == 0) return execution_error("unknown session: " + input->session_id);
            glz::generic structured = object();
            structured["session_id"] = input->session_id;
            structured["closed"] = true;
            return tool_result(std::move(structured));
        }
        return execution_error("unknown tool: " + call.name);
    }

    std::optional<glz::generic> handle(const RpcRequest &request) {
        if (request.method.starts_with("notifications/")) return std::nullopt;
        if (request.jsonrpc != "2.0") return rpc_error(request.id, -32600, "invalid JSON-RPC request");

        if (request.method == "server/discover") {
            glz::generic result = object();
            result["resultType"] = "complete";
            result["supportedVersions"] = parse_literal(R"json(["2026-07-28","2025-11-25"])json");
            result["capabilities"] = parse_literal(R"json({"tools":{"listChanged":false}})json");
            result["_meta"] =
                parse_literal(R"json({"io.modelcontextprotocol/serverInfo":{"name":"liminal-dev-mcp","version":"0.1.0"}})json");
            result["instructions"] =
                "Use session_create, session_apply, session_inspect, then session_close to reproduce Liminal UI scenarios.";
            result["cacheScope"] = "public";
            return rpc_result(request.id, std::move(result));
        }
        if (request.method == "initialize") {
            glz::generic result = object();
            result["protocolVersion"] = "2025-11-25";
            result["capabilities"] = parse_literal(R"json({"tools":{"listChanged":false}})json");
            result["serverInfo"] = parse_literal(R"json({"name":"liminal-dev-mcp","version":"0.1.0"})json");
            result["instructions"] = "Development-only deterministic Liminal UI inspection.";
            return rpc_result(request.id, std::move(result));
        }
        if (request.method == "ping") return rpc_result(request.id, object());
        if (request.method == "tools/list") {
            glz::generic result = object();
            result["resultType"] = "complete";
            result["tools"] = tools();
            result["cacheScope"] = "public";
            return rpc_result(request.id, std::move(result));
        }
        if (request.method == "tools/call") {
            auto call = decode<CallToolInput>(request.params);
            if (!call) return rpc_error(request.id, -32602, call.error());
            if (call->name != "session_create" && call->name != "session_apply" && call->name != "session_inspect" &&
                call->name != "session_close") {
                return rpc_error(request.id, -32602, "unknown tool: " + call->name);
            }
            return rpc_result(request.id, call_tool(*call));
        }
        return rpc_error(request.id, -32601, "method not found: " + request.method);
    }

    std::unordered_map<std::string, std::unique_ptr<tui::HeadlessSession>> sessions;
    u64 next_id = 1;
};

} // namespace

int run() {
    Server server;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto request = json::parse<RpcRequest>(line);
        glz::generic response;
        if (!request) {
            response = rpc_error({}, -32700, request.error().detail);
        } else {
            auto handled = server.handle(*request);
            if (!handled) continue;
            response = *std::move(handled);
        }
        auto encoded = json::to_string(response);
        if (!encoded) {
            std::fprintf(stderr, "cannot encode MCP response: %s\n", encoded.error().detail.c_str());
            return 1;
        }
        std::cout << *encoded << '\n' << std::flush;
    }
    return 0;
}

} // namespace liminal::dev_mcp

int main() {
    try {
        return liminal::dev_mcp::run();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "liminal-dev-mcp: %s\n", error.what());
        return 1;
    }
}
