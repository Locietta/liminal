#pragma once

#include <span>
#include <string>
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

/// Decode an already framed sequence of Anthropic server-sent events.
/// Network chunk framing is tested independently by lighter::http::sse.
Result<provider::ProviderCallCompletion> decode_stream(std::span<const lighter::http::sse::Event> events,
                                                       const provider::StreamCallbacks &callbacks = {}, std::string request_id = {});

} // namespace liminal::anthropic::protocol
