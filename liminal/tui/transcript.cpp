#include "transcript.h"

#include <type_traits>
#include <utility>

#include <lighter/utils/panic.h>

namespace liminal::tui {

void Transcript::apply(const Event &event, std::chrono::steady_clock::time_point now) {
    std::visit(
        [this, now](const auto &value) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, ToolStarted>) {
                apply_one(value, now);
            } else {
                apply_one(value);
            }
        },
        event);
}

void Transcript::apply_one(const PromptSubmitted &event) {
    append({.kind = BlockKind::USER, .state = BlockState::COMPLETED, .text = event.text});
}

void Transcript::apply_one(const AssistantTextDelta &event) {
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (block->kind == BlockKind::ASSISTANT && block->state == BlockState::STREAMING && block->output_item_id == event.item_id) {
            block->text += event.text;
            return;
        }
    }
    append({
        .kind = BlockKind::ASSISTANT,
        .state = BlockState::STREAMING,
        .text = event.text,
        .output_item_id = event.item_id,
    });
}

void Transcript::apply_one(const AssistantMessageCompleted &event) { finish_assistant(event.item_id, BlockState::COMPLETED, event.phase); }

void Transcript::apply_one(const ToolStarted &event, std::chrono::steady_clock::time_point now) {
    append({
        .kind = BlockKind::TOOL,
        .state = BlockState::RUNNING,
        .text = event.description.empty() ? event.name : event.description,
        .tool_name = event.name,
        .command = event.command,
        .call_id = event.call_id,
        .started_at = now,
    });
}

void Transcript::apply_one(const ToolCompleted &event) {
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (block->kind == BlockKind::TOOL && block->call_id == event.call_id) {
            lighter::check(block->state == BlockState::RUNNING, "completed tool block was not running");
            block->state = event.is_error ? BlockState::FAILED : BlockState::COMPLETED;
            if (!event.description.empty()) block->text = event.description;
            if (!event.command.empty()) block->command = event.command;
            block->detail = event.summary;
            return;
        }
    }
    lighter::panic("completed tool block was not found");
}

void Transcript::apply_one(const TaskCompleted &) { finish_streaming(BlockState::COMPLETED); }

void Transcript::apply_one(const TaskCancelled &) {
    finish_streaming(BlockState::CANCELLED);
    for (auto &block : blocks) {
        if (block.kind == BlockKind::TOOL && block.state == BlockState::RUNNING) {
            block.state = BlockState::CANCELLED;
        }
    }
    append({.kind = BlockKind::NOTICE, .state = BlockState::CANCELLED, .text = "Task cancelled"});
}

void Transcript::apply_one(const TaskFailed &event) {
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
    for (auto &block : blocks) {
        if (block.kind == BlockKind::ASSISTANT && block.state == BlockState::STREAMING) block.state = state;
    }
}

void Transcript::finish_assistant(std::string_view item_id, BlockState state, provider::MessagePhase phase) {
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (block->kind == BlockKind::ASSISTANT && block->output_item_id == item_id) {
            lighter::check(block->state == BlockState::STREAMING, "completed assistant block was not streaming");
            block->state = state;
            block->message_phase = phase;
            return;
        }
    }
}

Block &Transcript::append(Block block) {
    block.id = next_id++;
    blocks.push_back(std::move(block));
    return blocks.back();
}

} // namespace liminal::tui
