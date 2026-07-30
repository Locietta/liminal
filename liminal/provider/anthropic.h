#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/http/http.h>
#include <lighter/types.hpp>

#include "liminal/error.h"
#include "liminal/provider/common.h"
#include "liminal/provider/history.h"

namespace liminal::anthropic {

using namespace lighter::types;

/// Tag identifying this provider's OpaquePart payloads (thinking blocks and
/// their signatures, serialized in wire form).
inline constexpr std::string_view k_provider_tag = "anthropic";

struct ClientOptions {
    /// Sent as `x-api-key` (direct Anthropic API convention).
    std::string api_key;
    /// Sent as `Authorization: Bearer` (proxy/gateway convention). Either
    /// this or api_key must be set; both may be.
    std::string auth_token;
    std::string base_url = "https://api.anthropic.com";
    std::string model;
    u32 max_tokens = 8192;
    usize max_retries = 2;
    std::chrono::milliseconds initial_retry_delay{500};
};

/// Anthropic Messages API client, conforming to provider::ProviderFacade.
/// Wire types and the SSE accumulator are implementation details; the public
/// surface speaks the neutral transcript only.
struct Client {
    explicit Client(ClientOptions options);

    /// Streams one Messages API call to completion, invoking callbacks live.
    /// Retries transport/429/5xx failures with backoff, but only while no
    /// text has been surfaced through the callbacks.
    lighter::Task<provider::TurnResponse, Error> complete(const provider::History &history,
                                                          const std::vector<provider::ToolDefinition> &tools,
                                                          const provider::StreamCallbacks &callbacks);

    /// Anthropic has no remote compaction endpoint; summarizes locally
    /// through complete() (provider/compact.h).
    lighter::Task<void, Error> compact(provider::History &history, std::string_view instructions);

    ClientOptions options;
    lighter::http::Client http_client;

    // --- temporary bridge for the templated Agent --------------------------
    // Deleted when Agent de-templates onto pro::proxy<ProviderFacade>; the
    // facade methods above are the real interface.

    using History = provider::History;
    using Response = provider::TurnResponse;

    lighter::Task<provider::TurnResponse, Error> create_message(std::string model, u32 max_tokens, const History &history,
                                                                const std::vector<provider::ToolDefinition> &tools,
                                                                const provider::StreamCallbacks &callbacks = {}) {
        options.model = std::move(model);
        options.max_tokens = max_tokens;
        co_return co_await complete(history, tools, callbacks).or_fail();
    }

    lighter::Task<void, Error> compact_history(std::string model, History &history, std::string instructions) {
        options.model = std::move(model);
        co_return co_await compact(history, instructions).or_fail();
    }

    static void append_user(History &history, std::string prompt) { provider::append_user(history, std::move(prompt)); }
    static std::vector<const provider::ToolCall *> tool_calls(const Response &response) { return provider::tool_calls(response); }
    static bool is_terminal(const Response &response) { return response.stop == provider::StopKind::DONE; }
    static bool requires_tool_results(const Response &response) { return response.stop == provider::StopKind::NEEDS_TOOL_RESULTS; }
    static void append_response(History &history, Response response) { provider::append_response(history, std::move(response)); }
    static void append_tool_results(History &history, std::vector<provider::ToolResult> results) {
        provider::append_tool_results(history, std::move(results));
    }
};

} // namespace liminal::anthropic
