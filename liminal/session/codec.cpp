#include "codec.h"

#include <array>
#include <type_traits>

#include <glaze/json.hpp>

#include <lighter/async/vocab/outcome.h>

template <>
struct glz::meta<liminal::provider::MessagePhase> {
    using enum liminal::provider::MessagePhase;
    static constexpr auto value = enumerate("unspecified", UNSPECIFIED, "commentary", COMMENTARY, "final", FINAL);
};

template <>
struct glz::meta<liminal::provider::StopKind> {
    using enum liminal::provider::StopKind;
    static constexpr auto value = enumerate("done", DONE, "needs_tool_results", NEEDS_TOOL_RESULTS, "truncated", TRUNCATED, "refused",
                                            REFUSED, "context_exhausted", CONTEXT_EXHAUSTED, "other", OTHER);
};

template <>
struct glz::meta<liminal::session::ProviderCallLoopOutcome> {
    using enum liminal::session::ProviderCallLoopOutcome;
    static constexpr auto value = enumerate("follow_up", FOLLOW_UP, "terminal", TERMINAL, "failed", FAILED);
};

template <>
struct glz::meta<liminal::session::ProviderCallAbortReason> {
    using enum liminal::session::ProviderCallAbortReason;
    static constexpr auto value = enumerate("cancelled", CANCELLED, "failed", FAILED, "interrupted", INTERRUPTED);
};

template <>
struct glz::meta<liminal::session::TaskOutcome> {
    using enum liminal::session::TaskOutcome;
    static constexpr auto value = enumerate("completed", COMPLETED, "cancelled", CANCELLED, "failed", FAILED, "interrupted", INTERRUPTED);
};

template <>
struct glz::meta<liminal::session::CheckpointItem> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"input", "output"};
};

namespace liminal::session {

namespace {

template <typename T>
Result<std::string> encode(const T &value) {
    auto json = glz::write_json(value);
    if (!json) return lighter::outcome_error(Error::protocol("cannot encode session payload: " + glz::format_error(json.error())));
    return *std::move(json);
}

template <typename T>
Result<EntryPayload> decode(std::string_view json, EntryKind kind) {
    T value;
    if (auto error = glz::read_json(value, json)) {
        return lighter::outcome_error(
            Error::protocol("cannot decode " + std::string(entry_kind_name(kind)) + " payload: " + glz::format_error(error, json)));
    }
    return EntryPayload{std::move(value)};
}

} // namespace

std::string_view entry_kind_name(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::TASK_STARTED: return "TaskStarted";
        case EntryKind::OUTPUT_ITEM_COMPLETED: return "OutputItemCompleted";
        case EntryKind::PROVIDER_CALL_COMPLETED: return "ProviderCallCompleted";
        case EntryKind::PROVIDER_CALL_ABORTED: return "ProviderCallAborted";
        case EntryKind::TOOL_RESULTS: return "ToolResults";
        case EntryKind::TASK_FINISHED: return "TaskFinished";
        case EntryKind::CONTEXT_CHECKPOINT: return "ContextCheckpoint";
    }
    return "unknown";
}

Result<EncodedPayload> encode_payload(const EntryPayload &payload) {
    return std::visit(
        [](const auto &value) -> Result<EncodedPayload> {
            using T = std::remove_cvref_t<decltype(value)>;
            EncodedPayload encoded;
            if constexpr (std::same_as<T, TaskStarted>) {
                encoded.kind = EntryKind::TASK_STARTED;
                encoded.task_id = value.id;
            } else if constexpr (std::same_as<T, OutputItemCompleted>) {
                encoded.kind = EntryKind::OUTPUT_ITEM_COMPLETED;
                encoded.task_id = value.task_id;
                encoded.provider_call_id = value.provider_call_id;
            } else if constexpr (std::same_as<T, ProviderCallCompleted>) {
                encoded.kind = EntryKind::PROVIDER_CALL_COMPLETED;
                encoded.task_id = value.task_id;
                encoded.provider_call_id = value.id;
            } else if constexpr (std::same_as<T, ProviderCallAborted>) {
                encoded.kind = EntryKind::PROVIDER_CALL_ABORTED;
                encoded.task_id = value.task_id;
                encoded.provider_call_id = value.id;
            } else if constexpr (std::same_as<T, ToolResults>) {
                encoded.kind = EntryKind::TOOL_RESULTS;
                encoded.task_id = value.task_id;
                encoded.provider_call_id = value.provider_call_id;
            } else if constexpr (std::same_as<T, TaskFinished>) {
                encoded.kind = EntryKind::TASK_FINISHED;
                encoded.task_id = value.id;
            } else {
                encoded.kind = EntryKind::CONTEXT_CHECKPOINT;
            }
            auto json = encode(value);
            if (!json) return lighter::outcome_error(std::move(json).error());
            encoded.json = *std::move(json);
            return encoded;
        },
        payload);
}

Result<EntryPayload> decode_payload(EntryKind kind, u32 version, std::string_view json) {
    if (version != 1) {
        return lighter::outcome_error(
            Error::protocol("unsupported " + std::string(entry_kind_name(kind)) + " payload version " + std::to_string(version)));
    }
    switch (kind) {
        case EntryKind::TASK_STARTED: return decode<TaskStarted>(json, kind);
        case EntryKind::OUTPUT_ITEM_COMPLETED: return decode<OutputItemCompleted>(json, kind);
        case EntryKind::PROVIDER_CALL_COMPLETED: return decode<ProviderCallCompleted>(json, kind);
        case EntryKind::PROVIDER_CALL_ABORTED: return decode<ProviderCallAborted>(json, kind);
        case EntryKind::TOOL_RESULTS: return decode<ToolResults>(json, kind);
        case EntryKind::TASK_FINISHED: return decode<TaskFinished>(json, kind);
        case EntryKind::CONTEXT_CHECKPOINT: return decode<ContextCheckpoint>(json, kind);
    }
    return lighter::outcome_error(Error::protocol("unknown session entry kind " + std::to_string(static_cast<u32>(kind))));
}

} // namespace liminal::session
