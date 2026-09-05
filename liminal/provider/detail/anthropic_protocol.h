#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/http/sse.h>

#include <liminal/error.h>
#include <liminal/provider/anthropic.h>
#include <liminal/provider/common.h>
#include <liminal/provider/history.h>

namespace liminal::anthropic::protocol {

/// Encode one Anthropic Messages API request from the neutral transcript.
Result<std::string> encode_complete_request(const provider::History &history, const std::vector<provider::ToolDefinition> &tools,
                                            const ClientOptions &options);

/// Decode a non-2xx response body into an error, classifying context-window
/// rejections with ErrorCode::CONTEXT_EXHAUSTED.
Error decode_status_error(int status, std::string_view body, std::string request_id,
                          std::optional<std::chrono::milliseconds> retry_after = std::nullopt);

/// Decode an already framed sequence of Anthropic server-sent events.
/// Network chunk framing is tested independently by lighter::http::sse.
Result<provider::ProviderCallCompletion> decode_stream(std::span<const lighter::http::sse::Event> events,
                                                       const provider::StreamCallbacks &callbacks = {}, std::string request_id = {});

} // namespace liminal::anthropic::protocol
