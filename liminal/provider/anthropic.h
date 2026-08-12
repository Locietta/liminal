#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/http/http.h>
#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/auth.h>
#include <liminal/provider/common.h>
#include <liminal/provider/history.h>

namespace liminal::anthropic {

using namespace lighter::types;

/// Tag identifying this provider's OpaquePart payloads (thinking blocks and
/// their signatures, serialized in wire form).
inline constexpr std::string_view k_provider_tag = "anthropic";

struct ClientOptions {
    provider::AuthResolver auth;
    std::string base_url = "https://api.anthropic.com";
    std::string model;
    std::optional<std::string> reasoning_effort;
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
    lighter::Task<provider::ProviderCallCompletion, Error> complete(const provider::History &history,
                                                                    const std::vector<provider::ToolDefinition> &tools,
                                                                    const provider::StreamCallbacks &callbacks);

    /// Anthropic has no remote compaction endpoint; summarizes locally
    /// through complete() (provider/compact.h).
    lighter::Task<void, Error> compact(provider::History &history, std::string_view instructions);

    ClientOptions options;
    lighter::http::Client http_client;
};

/// Lists every page from the configured Anthropic-compatible Models API.
lighter::Task<std::vector<provider::DiscoveredModel>, Error> list_models(ClientOptions options);

} // namespace liminal::anthropic
