#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/json.hpp>
#include <proxy/proxy.h>

#include <lighter/async/io/loop.h>
#include <lighter/types.hpp>

#include <liminal/agent/agent.h>
#include <liminal/event.h>
#include <liminal/provider/provider.h>

namespace {

using namespace lighter::types;
using namespace liminal;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

struct FakeProvider {
    lighter::Task<provider::TurnResponse, Error> complete(const provider::History &, const std::vector<provider::ToolDefinition> &,
                                                          const provider::StreamCallbacks &callbacks) {
        if (calls++ == 0) {
            callbacks.on_text_delta("checking");
            glz::generic input;
            auto parse_error = glz::read_json(input, R"({"path":"README.md"})");
            require(!parse_error, "failed to create read_file input");

            provider::TurnResponse response{.stop = provider::StopKind::NEEDS_TOOL_RESULTS};
            response.parts.push_back(provider::TextPart{.text = "checking"});
            response.parts.push_back(provider::ToolCall{
                .id = "call-1",
                .name = "read_file",
                .input = std::move(input),
            });
            co_return response;
        }

        callbacks.on_text_delta("done");
        provider::TurnResponse response{.stop = provider::StopKind::DONE};
        response.parts.push_back(provider::TextPart{.text = "done"});
        co_return response;
    }

    lighter::Task<void, Error> compact(provider::History &, std::string_view) { co_return; }

    usize calls = 0;
};

i32 run_all() {
    ToolSet tools(std::filesystem::current_path().string());
    ProviderChoice choice{
        .handle = pro::make_proxy<provider::ProviderFacade, FakeProvider>(),
        .name = "fake",
        .model = "test",
    };
    Agent agent(std::move(choice), tools);

    std::vector<Event> events;
    EventSink sink = [&events](const Event &event) { events.push_back(event); };

    lighter::EventLoop loop;
    auto task = agent.run_turn("inspect", sink);
    loop.schedule(task);
    loop.run();
    auto outcome = task.result();

    require(outcome.has_value(), "agent turn failed");
    require(events.size() == 7, "agent emitted an unexpected event count");
    require(std::holds_alternative<AssistantTextDelta>(events[0]), "text delta must be the first response event");
    require(std::holds_alternative<AssistantSegmentCompleted>(events[1]), "assistant segment must finish before its tool");
    require(std::holds_alternative<ToolStarted>(events[2]), "tool start event is missing");
    require(std::holds_alternative<ToolCompleted>(events[3]), "tool completion event is missing");
    require(std::holds_alternative<AssistantTextDelta>(events[4]), "continuation text delta is missing");
    require(std::holds_alternative<AssistantSegmentCompleted>(events[5]), "continuation segment completion is missing");
    require(std::holds_alternative<TurnCompleted>(events[6]), "turn completion event is missing");
    require(agent.history.size() == 4, "transactional provider history has the wrong number of items");
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
