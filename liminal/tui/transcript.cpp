#include "transcript.h"

#include <utility>

#include <lighter/utils/panic.h>

namespace liminal::tui {

void Transcript::apply(const Event &event) {
    std::visit([this](const auto &value) { apply_one(value); }, event);
}

void Transcript::apply_one(const PromptSubmitted &event) {
    append({.kind = BlockKind::USER, .state = BlockState::COMPLETED, .text = event.text});
}

void Transcript::apply_one(const AssistantTextDelta &event) {
    if (!blocks.empty() && blocks.back().kind == BlockKind::ASSISTANT && blocks.back().state == BlockState::STREAMING) {
        blocks.back().text += event.text;
        return;
    }
    append({.kind = BlockKind::ASSISTANT, .state = BlockState::STREAMING, .text = event.text});
}

void Transcript::apply_one(const AssistantSegmentCompleted &) { finish_streaming(BlockState::COMPLETED); }

void Transcript::apply_one(const ToolStarted &event) {
    finish_streaming(BlockState::COMPLETED);
    append({
        .kind = BlockKind::TOOL,
        .state = BlockState::RUNNING,
        .text = event.description.empty() ? event.name : event.description,
        .call_id = event.call_id,
    });
}

void Transcript::apply_one(const ToolCompleted &event) {
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (block->kind == BlockKind::TOOL && block->call_id == event.call_id) {
            lighter::check(block->state == BlockState::RUNNING, "completed tool block was not running");
            block->state = event.is_error ? BlockState::FAILED : BlockState::COMPLETED;
            if (!event.description.empty()) block->text = event.description;
            block->detail = event.summary;
            return;
        }
    }
    lighter::panic("completed tool block was not found");
}

void Transcript::apply_one(const TurnCompleted &) { finish_streaming(BlockState::COMPLETED); }

void Transcript::apply_one(const TurnCancelled &) {
    finish_streaming(BlockState::CANCELLED);
    for (auto &block : blocks) {
        if (block.kind == BlockKind::TOOL && block.state == BlockState::RUNNING) {
            block.state = BlockState::CANCELLED;
        }
    }
    append({.kind = BlockKind::NOTICE, .state = BlockState::CANCELLED, .text = "Turn cancelled"});
}

void Transcript::apply_one(const TurnFailed &event) {
    finish_streaming(BlockState::FAILED);
    for (auto &block : blocks) {
        if (block.kind == BlockKind::TOOL && block.state == BlockState::RUNNING) {
            block.state = BlockState::FAILED;
        }
    }
    append({.kind = BlockKind::NOTICE, .state = BlockState::FAILED, .text = event.message});
}

void Transcript::apply_one(const SessionNotice &event) {
    append({.kind = BlockKind::NOTICE, .state = BlockState::COMPLETED, .text = event.text});
}

void Transcript::apply_one(const ModelSelected &event) {
    auto selection = event.name;
    if (event.effort) selection += "@" + *event.effort;
    append({.kind = BlockKind::NOTICE, .state = BlockState::COMPLETED, .text = "Model: " + selection});
}

void Transcript::finish_streaming(BlockState state) {
    if (!blocks.empty() && blocks.back().kind == BlockKind::ASSISTANT && blocks.back().state == BlockState::STREAMING) {
        blocks.back().state = state;
    }
}

Block &Transcript::append(Block block) {
    block.id = next_id++;
    blocks.push_back(std::move(block));
    return blocks.back();
}

} // namespace liminal::tui
