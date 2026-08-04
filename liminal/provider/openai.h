#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/http/http.h>
#include <lighter/types.hpp>

#include "liminal/error.h"
#include "liminal/provider/auth.h"
#include "liminal/provider/common.h"
#include "liminal/provider/history.h"

namespace liminal::openai {

using namespace lighter::types;

/// Tag identifying this provider's OpaquePart payloads (reasoning items with
/// encrypted content, compaction items - serialized in wire form).
inline constexpr std::string_view k_provider_tag = "openai";

struct ClientOptions {
    provider::AuthResolver auth;
    std::string base_url = "https://api.openai.com/v1";
    std::optional<std::string> models_client_version;
    std::string model;
    std::optional<std::string> reasoning_effort;
    std::optional<u32> max_output_tokens = 8192;
    bool allow_missing_event_stream_content_type = false;
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
};

/// Lists models exposed by the configured OpenAI-compatible endpoint.
lighter::Task<std::vector<provider::DiscoveredModel>, Error> list_models(ClientOptions options);

} // namespace liminal::openai
