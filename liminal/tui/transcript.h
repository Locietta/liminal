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
    std::chrono::steady_clock::time_point started_at;
};

struct Transcript {
    void apply(const Event &event, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    std::vector<Block> blocks;

private:
    void apply_one(const PromptSubmitted &event);
    void apply_one(const AssistantTextDelta &event);
    void apply_one(const AssistantSegmentCompleted &event);
    void apply_one(const ToolStarted &event, std::chrono::steady_clock::time_point now);
    void apply_one(const ToolCompleted &event);
    void apply_one(const TurnCompleted &event);
    void apply_one(const TurnCancelled &event);
    void apply_one(const TurnFailed &event);
    void apply_one(const SessionNotice &event);
    void apply_one(const ModelSelected &event);
    void finish_streaming(BlockState state);
    Block &append(Block block);

    u64 next_id = 1;
};

} // namespace liminal::tui
