#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/types.hpp>

#include <liminal/event.h>
#include <liminal/tui/transcript.h>

namespace {

using namespace liminal;
using namespace lighter::types;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void check_stream_consolidation() {
    tui::Transcript transcript;
    transcript.apply(PromptSubmitted{.text = "hello"});
    transcript.apply(AssistantTextDelta{.text = "one"});
    transcript.apply(AssistantTextDelta{.text = " two"});
    transcript.apply(AssistantSegmentCompleted{});

    require(transcript.blocks.size() == 2, "adjacent assistant deltas must share one block");
    require(transcript.blocks[0].kind == tui::BlockKind::USER && transcript.blocks[0].text == "hello",
            "submitted prompts must enter the UI transcript");
    require(transcript.blocks[1].kind == tui::BlockKind::ASSISTANT && transcript.blocks[1].text == "one two",
            "assistant text must consolidate in arrival order");
    require(transcript.blocks[1].state == tui::BlockState::COMPLETED, "a completed segment must become stable");
}

void check_tool_lifecycle() {
    tui::Transcript transcript;
    const auto started_at = std::chrono::steady_clock::time_point(std::chrono::seconds(7));
    transcript.apply(AssistantTextDelta{.text = "checking"});
    transcript.apply(AssistantSegmentCompleted{});
    transcript.apply(ToolStarted{.call_id = "call-1", .name = "read_file", .description = "Read README.md"}, started_at);
    transcript.apply(ToolCompleted{
        .call_id = "call-1",
        .name = "read_file",
        .description = "Read README.md",
        .summary = "5 lines · 189 bytes",
        .is_error = false,
    });

    require(transcript.blocks.size() == 2, "a tool must follow, not replace, its assistant segment");
    require(transcript.blocks[0].state == tui::BlockState::COMPLETED, "assistant output must stabilize before a tool block");
    require(transcript.blocks[1].kind == tui::BlockKind::TOOL && transcript.blocks[1].call_id == "call-1",
            "tool identity must remain inspectable");
    require(transcript.blocks[1].text == "Read README.md" && transcript.blocks[1].tool_name == "read_file" &&
                transcript.blocks[1].detail == "5 lines · 189 bytes" && transcript.blocks[1].started_at == started_at,
            "tool blocks must retain their specific invocation and bounded completion detail");
    require(transcript.blocks[1].state == tui::BlockState::COMPLETED, "tool completion must update the matching block");
}

void check_cancelled_partial_output() {
    tui::Transcript streaming;
    streaming.apply(AssistantTextDelta{.text = "partial"});
    streaming.apply(TurnCancelled{});
    require(streaming.blocks[0].state == tui::BlockState::CANCELLED && streaming.blocks[0].text == "partial",
            "streaming assistant output must remain visible after cancellation");

    tui::Transcript transcript;
    transcript.apply(AssistantTextDelta{.text = "partial"});
    transcript.apply(AssistantSegmentCompleted{});
    transcript.apply(ToolStarted{.call_id = "call-1", .name = "exec_command"});
    transcript.apply(TurnCancelled{});

    require(transcript.blocks.size() == 3, "cancellation must preserve partial output, tool state, and a notice");
    require(transcript.blocks[0].state == tui::BlockState::COMPLETED && transcript.blocks[0].text == "partial",
            "a finalized assistant segment must remain stable after cancellation");
    require(transcript.blocks[1].kind == tui::BlockKind::TOOL && transcript.blocks[1].state == tui::BlockState::CANCELLED,
            "an interrupted tool must not remain running");
    require(transcript.blocks[2].kind == tui::BlockKind::NOTICE && transcript.blocks[2].state == tui::BlockState::CANCELLED,
            "cancellation must be recorded as a semantic notice");
}

void check_typed_model_selection() {
    tui::Transcript transcript;
    transcript.apply(ModelSelected{.name = "test-model", .effort = "high"});
    require(transcript.blocks.size() == 1 && transcript.blocks[0].kind == tui::BlockKind::NOTICE &&
                transcript.blocks[0].text == "Model: test-model@high",
            "model selection must remain inspectable as a typed transcript event");
}

i32 run_all() {
    check_stream_consolidation();
    check_tool_lifecycle();
    check_cancelled_partial_output();
    check_typed_model_selection();
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
