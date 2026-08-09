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

#include <liminal/dev_mcp/live_session.h>
#include <liminal/tui/headless.h>

namespace liminal::dev_mcp {

namespace json = lighter::codec::json;
using namespace lighter::types;

namespace {

constexpr usize k_max_sessions = 32;
constexpr usize k_max_live_sessions = 4;
constexpr usize k_max_actions_per_call = 1000;
constexpr usize k_max_live_input_bytes_per_call = 4 * 1024 * 1024;
constexpr i64 k_max_live_wait_ms_per_action = 30'000;
constexpr i64 k_max_live_wait_ms_per_call = 30'000;

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
    std::string cwd = ".";
};

struct ApplyInput {
    std::string session_id;
    std::vector<glz::generic> actions;
};

struct LiveAction {
    std::string type;
    std::string text;
    std::string key;
    i32 columns = 0;
    i32 rows = 0;
    i64 milliseconds = 0;
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

glz::generic snapshot_value(const LiveSnapshot &snapshot) {
    auto encoded = json::to_string(snapshot);
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
        "title":"Create Liminal session",
        "description":"Create either a deterministic in-memory UI session or a real Liminal process owned by this MCP server under ConPTY/PTY.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"driver":{"type":"string","enum":["tui.headless","liminal.pty"],"default":"tui.headless"},"columns":{"type":"integer","minimum":1,"maximum":500,"default":80},"rows":{"type":"integer","minimum":1,"maximum":200,"default":24},"now_ms":{"type":"integer","minimum":0,"default":0},"cwd":{"type":"string","description":"Working directory for liminal.pty; defaults to the MCP server working directory.","default":"."}}},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":true}
      },
      {
        "name":"session_apply",
        "title":"Operate Liminal session",
        "description":"Apply headless actions, or operate a real Liminal terminal with write, prompt, key, resize, wait, and terminate actions. A call may wait for at most 30000 milliseconds total and write at most 4 MiB total. Supported key names: enter, escape, backspace, ctrl_c, ctrl_j, tab, arrows, page_up, page_down, home, end, and delete.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string"},"actions":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{"type":{"type":"string"},"text":{"type":"string","maxLength":1048576},"key":{"type":"string"},"call_id":{"type":"string"},"name":{"type":"string"},"command":{"type":"string","maxLength":2048},"effort":{"type":["string","null"]},"columns":{"type":"integer"},"rows":{"type":"integer"},"amount":{"type":"integer"},"milliseconds":{"type":"integer"},"is_error":{"type":"boolean"}},"required":["type"],"additionalProperties":false}}},"required":["session_id","actions"]},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
      },
      {
        "name":"session_inspect",
        "title":"Inspect Liminal session",
        "description":"Return the current deterministic UI snapshot, or a live process snapshot with state, cumulative ANSI output, and decoded visible terminal rows.",
        "inputSchema":{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string"}},"required":["session_id"]},
        "outputSchema":{"type":"object"},
        "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
      },
      {
        "name":"session_close",
        "title":"Close Liminal session",
        "description":"Release a session and terminate its MCP-owned Liminal process if it is still running.",
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
            if (input->driver != "tui.headless" && input->driver != "liminal.pty") {
                return execution_error("unsupported driver: " + input->driver);
            }
            if (headless_sessions.size() + live_sessions.size() >= k_max_sessions) {
                return execution_error("server session limit exceeded");
            }
            if (input->driver == "liminal.pty" && live_sessions.size() >= k_max_live_sessions) {
                return execution_error("live session limit exceeded");
            }
            if (input->columns < 1 || input->columns > 500 || input->rows < 1 || input->rows > 200 || input->now_ms < 0) {
                return execution_error("invalid terminal size or virtual time");
            }
            auto id = "session-" + std::to_string(next_id++);
            glz::generic structured = object();
            structured["session_id"] = id;
            if (input->driver == "tui.headless") {
                auto session = std::make_unique<tui::HeadlessSession>(input->columns, input->rows, input->now_ms);
                structured["capabilities"] = parse_literal(
                    R"json({"driver":"tui.headless","version":"1","actions":true,"virtual_clock":true,"snapshots":true,"process":false,"network":false,"workspace":false})json");
                structured["snapshot"] = snapshot_value(*session);
                headless_sessions.emplace(id, std::move(session));
            } else if (input->driver == "liminal.pty") {
                auto session = LiveSession::create(input->cwd, input->columns, input->rows);
                if (!session) return execution_error(session.error());
                auto snapshot = (*session)->inspect();
                if (!snapshot) return execution_error(snapshot.error());
                structured["capabilities"] = parse_literal(
                    R"json({"driver":"liminal.pty","version":"1","actions":true,"virtual_clock":false,"snapshots":true,"process":true,"network":true,"workspace":true,"authenticated":true})json");
                structured["snapshot"] = snapshot_value(*snapshot);
                live_sessions.emplace(id, *std::move(session));
            }
            return tool_result(std::move(structured));
        }

        auto get_headless = [this](std::string_view id) -> tui::HeadlessSession * {
            const auto found = headless_sessions.find(std::string(id));
            return found == headless_sessions.end() ? nullptr : found->second.get();
        };
        auto get_live = [this](std::string_view id) -> LiveSession * {
            const auto found = live_sessions.find(std::string(id));
            return found == live_sessions.end() ? nullptr : found->second.get();
        };

        if (call.name == "session_apply") {
            auto input = decode<ApplyInput>(call.arguments);
            if (!input) return execution_error(input.error());
            if (input->actions.size() > k_max_actions_per_call) return execution_error("action batch limit exceeded");
            glz::generic structured = object();
            structured["session_id"] = input->session_id;
            if (auto *session = get_headless(input->session_id)) {
                std::vector<tui::HeadlessAction> actions;
                actions.reserve(input->actions.size());
                for (const auto &value : input->actions) {
                    auto action = decode<tui::HeadlessAction>(value);
                    if (!action) return execution_error(action.error());
                    actions.push_back(*std::move(action));
                }
                if (auto applied = session->apply(actions); !applied) return execution_error(applied.error());
                structured["snapshot"] = snapshot_value(*session);
            } else if (auto *session = get_live(input->session_id)) {
                std::vector<LiveAction> actions;
                actions.reserve(input->actions.size());
                usize total_input_bytes = 0;
                i64 total_wait_ms = 0;
                for (const auto &value : input->actions) {
                    auto action = decode<LiveAction>(value);
                    if (!action) return execution_error(action.error());
                    if (action->type == "write" || action->type == "prompt") {
                        if (action->text.size() > 1024 * 1024) {
                            return execution_error(action->type + " text limit exceeded");
                        }
                        const auto input_bytes = action->text.size() + (action->type == "prompt" ? 1 : 0);
                        if (input_bytes > k_max_live_input_bytes_per_call - total_input_bytes) {
                            return execution_error("total input byte budget exceeded");
                        }
                        total_input_bytes += input_bytes;
                    } else if (action->type == "key") {
                        if (!LiveSession::supports_key(action->key)) {
                            return execution_error("unsupported key: " + action->key);
                        }
                    } else if (action->type == "resize") {
                        if (action->columns < 1 || action->columns > 500 || action->rows < 1 || action->rows > 200) {
                            return execution_error("invalid terminal size");
                        }
                    } else if (action->type == "wait") {
                        if (action->milliseconds < 0 || action->milliseconds > k_max_live_wait_ms_per_action) {
                            return execution_error("wait must be between 0 and 30000 milliseconds");
                        }
                        if (action->milliseconds > k_max_live_wait_ms_per_call - total_wait_ms) {
                            return execution_error("total wait budget exceeded");
                        }
                        total_wait_ms += action->milliseconds;
                    } else if (action->type != "terminate") {
                        return execution_error("unsupported liminal.pty action: " + action->type);
                    }
                    actions.push_back(*std::move(action));
                }

                for (const auto &action : actions) {
                    std::expected<void, std::string> applied;
                    if (action.type == "write") {
                        applied = session->write(action.text);
                    } else if (action.type == "prompt") {
                        applied = session->write(action.text + "\r");
                    } else if (action.type == "key") {
                        applied = session->key(action.key);
                    } else if (action.type == "resize") {
                        applied = session->resize(action.columns, action.rows);
                    } else if (action.type == "wait") {
                        applied = session->wait(std::chrono::milliseconds(action.milliseconds));
                    } else {
                        applied = session->terminate();
                    }
                    if (!applied) return execution_error(applied.error());
                }
                auto snapshot = session->inspect();
                if (!snapshot) return execution_error(snapshot.error());
                structured["snapshot"] = snapshot_value(*snapshot);
            } else {
                return execution_error("unknown session: " + input->session_id);
            }
            return tool_result(std::move(structured));
        }
        if (call.name == "session_inspect") {
            auto input = decode<SessionInput>(call.arguments);
            if (!input) return execution_error(input.error());
            glz::generic structured = object();
            structured["session_id"] = input->session_id;
            if (auto *session = get_headless(input->session_id)) {
                structured["snapshot"] = snapshot_value(*session);
            } else if (auto *session = get_live(input->session_id)) {
                auto snapshot = session->inspect();
                if (!snapshot) return execution_error(snapshot.error());
                structured["snapshot"] = snapshot_value(*snapshot);
            } else {
                return execution_error("unknown session: " + input->session_id);
            }
            return tool_result(std::move(structured));
        }
        if (call.name == "session_close") {
            auto input = decode<SessionInput>(call.arguments);
            if (!input) return execution_error(input.error());
            const auto removed = headless_sessions.erase(input->session_id) + live_sessions.erase(input->session_id);
            if (removed == 0) return execution_error("unknown session: " + input->session_id);
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
                "Use tui.headless for deterministic rendering tests or liminal.pty to operate the real built Liminal CLI. "
                "Always close live sessions when finished.";
            result["cacheScope"] = "public";
            return rpc_result(request.id, std::move(result));
        }
        if (request.method == "initialize") {
            glz::generic result = object();
            result["protocolVersion"] = "2025-11-25";
            result["capabilities"] = parse_literal(R"json({"tools":{"listChanged":false}})json");
            result["serverInfo"] = parse_literal(R"json({"name":"liminal-dev-mcp","version":"0.1.0"})json");
            result["instructions"] = "Development-only deterministic and live Liminal CLI operation.";
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

    std::unordered_map<std::string, std::unique_ptr<tui::HeadlessSession>> headless_sessions;
    std::unordered_map<std::string, std::unique_ptr<LiveSession>> live_sessions;
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
