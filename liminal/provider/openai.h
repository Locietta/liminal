#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/http/http.h>
#include <lighter/types.hpp>

#include "liminal/error.h"
#include "liminal/provider/common.h"
#include "liminal/provider/history.h"

namespace liminal::openai {

using namespace lighter::types;

/// Tag identifying this provider's OpaquePart payloads (reasoning items with
/// encrypted content, compaction items - serialized in wire form).
inline constexpr std::string_view k_provider_tag = "openai";

struct ClientOptions {
    std::string api_key;
    std::string organization;
    std::string project;
    std::string base_url = "https://api.openai.com/v1";
    std::string model;
    u32 max_output_tokens = 8192;
    usize max_retries = 2;
    std::chrono::milliseconds initial_retry_delay{500};
};

/// OpenAI Responses API client, conforming to provider::ProviderFacade. Also
/// usable against OpenAI-compatible gateways; anything OpenAI-proprietary
/// (remote compaction) degrades gracefully when the gateway lacks it.
struct Client {
    explicit Client(ClientOptions options);

    /// Streams one Responses API call to completion, invoking callbacks live.
    /// Retries transport/429/5xx failures with backoff, but only while no
    /// text has been surfaced through the callbacks.
    lighter::Task<provider::TurnResponse, Error> complete(const provider::History &history,
                                                          const std::vector<provider::ToolDefinition> &tools,
                                                          const provider::StreamCallbacks &callbacks);

    /// Tries OpenAI's native `POST /responses/compact` first (stateless remote
    /// compaction returning an opaque encrypted item). Gateways that lack the
    /// endpoint (404/400/501) fall back to generic local summarization.
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
        options.max_output_tokens = max_tokens;
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

} // namespace liminal::openai
