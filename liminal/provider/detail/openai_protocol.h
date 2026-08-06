#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/http/sse.h>

#include <liminal/error.h>
#include <liminal/provider/common.h>
#include <liminal/provider/history.h>
#include <liminal/provider/openai.h>

namespace liminal::openai::protocol {

/// Encode one stateless Responses API request from the neutral transcript.
Result<std::string> encode_complete_request(const provider::History &history, const std::vector<provider::ToolDefinition> &tools,
                                            const ClientOptions &options);

/// Encode the provider-native compaction request for a conversation without
/// its immutable instruction prefix.
Result<std::string> encode_compact_request(const provider::History &conversation, std::string_view instructions,
                                           const ClientOptions &options);

/// Decode an already framed sequence of Responses API server-sent events.
/// Network chunk framing is tested independently by lighter::http::sse.
Result<provider::TurnResponse> decode_stream(std::span<const lighter::http::sse::Event> events,
                                             const provider::StreamCallbacks &callbacks = {}, std::string request_id = {});

} // namespace liminal::openai::protocol
