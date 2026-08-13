#include "hydration.h"

#include <map>
#include <tuple>
#include <utility>

namespace liminal::tui {

namespace {

using ToolKey = std::tuple<u64, u64, std::string>;

std::string assistant_text(const provider::AssistantMessageItem &message) {
    std::string text;
    for (const auto &part : message.parts) text += part.text;
    return text;
}

std::string abort_notice(const session::ProviderCallAborted &aborted) {
    switch (aborted.reason) {
        case session::ProviderCallAbortReason::CANCELLED: return "Provider call cancelled: " + aborted.detail;
        case session::ProviderCallAbortReason::FAILED: return "Provider call failed: " + aborted.detail;
        case session::ProviderCallAbortReason::INTERRUPTED: return "Provider call interrupted: " + aborted.detail;
    }
    return "Provider call ended unexpectedly";
}

} // namespace

std::vector<Block> project_transcript(const session::Session &session, const ToolSet &tools) {
    const auto branch = session.active_branch();
    std::map<ToolKey, provider::ToolResult> results;
    for (const auto *entry : branch) {
        const auto *batch = std::get_if<session::ToolResults>(&entry->payload);
        if (!batch) continue;
        for (const auto &result : batch->results) {
            results[{batch->task_id.value, batch->provider_call_id.value, result.call_id}] = result;
        }
    }

    std::vector<Block> blocks;
    for (const auto *entry : branch) {
        if (const auto *started = std::get_if<session::TaskStarted>(&entry->payload)) {
            blocks.push_back({.kind = BlockKind::USER, .state = BlockState::COMPLETED, .text = started->text});
            continue;
        }
        if (const auto *output = std::get_if<session::OutputItemCompleted>(&entry->payload)) {
            if (const auto *message = std::get_if<provider::AssistantMessageItem>(&output->item)) {
                auto text = assistant_text(*message);
                if (!text.empty()) {
                    blocks.push_back({
                        .kind = BlockKind::ASSISTANT,
                        .state = BlockState::COMPLETED,
                        .text = std::move(text),
                        .output_item_id = message->id.value,
                        .message_phase = message->phase,
                    });
                }
            } else if (const auto *tool = std::get_if<provider::ToolCallItem>(&output->item)) {
                const auto presentation = tools.describe(tool->call);
                auto result = results.find({output->task_id.value, output->provider_call_id.value, tool->call.id});
                Block block{
                    .kind = BlockKind::TOOL,
                    .state = BlockState::FAILED,
                    .text = presentation.description.empty() ? tool->call.name : presentation.description,
                    .tool_name = tool->call.name,
                    .command = presentation.command,
                    .call_id = tool->call.id,
                    .output_item_id = tool->id.value,
                };
                if (result != results.end()) {
                    block.state = result->second.is_error ? BlockState::FAILED : BlockState::COMPLETED;
                    block.detail = tools.summarize(tool->call, result->second);
                } else {
                    block.detail = "Result was not recorded";
                }
                blocks.push_back(std::move(block));
            }
            continue;
        }
        if (const auto *aborted = std::get_if<session::ProviderCallAborted>(&entry->payload)) {
            blocks.push_back({.kind = BlockKind::NOTICE, .state = BlockState::FAILED, .text = abort_notice(*aborted)});
            continue;
        }
        if (const auto *finished = std::get_if<session::TaskFinished>(&entry->payload)) {
            switch (finished->outcome) {
                case session::TaskOutcome::COMPLETED: break;
                case session::TaskOutcome::CANCELLED:
                    blocks.push_back({.kind = BlockKind::NOTICE, .state = BlockState::CANCELLED, .text = "Task cancelled"});
                    break;
                case session::TaskOutcome::FAILED:
                    blocks.push_back({.kind = BlockKind::NOTICE, .state = BlockState::FAILED, .text = "Task failed"});
                    break;
                case session::TaskOutcome::INTERRUPTED:
                    blocks.push_back({.kind = BlockKind::NOTICE, .state = BlockState::FAILED, .text = "Task interrupted by process exit"});
                    break;
            }
            continue;
        }
        if (std::holds_alternative<session::ContextCheckpoint>(entry->payload)) {
            blocks.push_back({.kind = BlockKind::NOTICE, .state = BlockState::COMPLETED, .text = "History compacted"});
        }
    }
    return blocks;
}

} // namespace liminal::tui
