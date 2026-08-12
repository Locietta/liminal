#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <lighter/types.hpp>

#include <liminal/event.h>

namespace liminal::tui {

using namespace lighter::types;

enum struct BlockKind {
    USER,
    ASSISTANT,
    TOOL,
    NOTICE,
};

enum struct BlockState {
    STREAMING,
    RUNNING,
    COMPLETED,
    CANCELLED,
    FAILED,
};

/// Width-independent source block. Layout rows are intentionally absent from
/// this model and will remain disposable renderer projections.
struct Block {
    u64 id = 0;
    BlockKind kind = BlockKind::NOTICE;
    BlockState state = BlockState::COMPLETED;
    std::string text;
    std::string detail;
    std::string tool_name;
    std::string command;
    std::string call_id;
    std::string output_item_id;
    provider::MessagePhase message_phase = provider::MessagePhase::UNSPECIFIED;
    std::chrono::steady_clock::time_point started_at;
};

struct Transcript {
    void apply(const Event &event, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    std::vector<Block> blocks;

private:
    void apply_one(const PromptSubmitted &event);
    void apply_one(const AssistantTextDelta &event);
    void apply_one(const AssistantMessageCompleted &event);
    void apply_one(const ToolStarted &event, std::chrono::steady_clock::time_point now);
    void apply_one(const ToolCompleted &event);
    void apply_one(const TaskCompleted &event);
    void apply_one(const TaskCancelled &event);
    void apply_one(const TaskFailed &event);
    void apply_one(const SessionNotice &event);
    void apply_one(const ModelSelected &event);
    void finish_streaming(BlockState state);
    void finish_assistant(std::string_view item_id, BlockState state, provider::MessagePhase phase);
    Block &append(Block block);

    u64 next_id = 1;
};

} // namespace liminal::tui
