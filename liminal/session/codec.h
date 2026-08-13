#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/session/session.h>

namespace liminal::session {

using namespace lighter::types;

enum struct EntryKind : u32 {
    TASK_STARTED = 1,
    OUTPUT_ITEM_COMPLETED = 2,
    PROVIDER_CALL_COMPLETED = 3,
    PROVIDER_CALL_ABORTED = 4,
    TOOL_RESULTS = 5,
    TASK_FINISHED = 6,
    CONTEXT_CHECKPOINT = 7,
};

struct EncodedPayload {
    EntryKind kind = EntryKind::TASK_STARTED;
    u32 version = 1;
    std::string json;
    std::optional<TaskId> task_id;
    std::optional<ProviderCallId> provider_call_id;
};

Result<EncodedPayload> encode_payload(const EntryPayload &payload);
Result<EntryPayload> decode_payload(EntryKind kind, u32 version, std::string_view json);
std::string_view entry_kind_name(EntryKind kind) noexcept;

} // namespace liminal::session
