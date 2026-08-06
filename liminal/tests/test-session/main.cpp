#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <lighter/types.hpp>

#include <liminal/context/context.h>
#include <liminal/session/session.h>

namespace {

using namespace lighter::types;
using namespace liminal;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_append_only_branching() {
    session::Session log({.value = 7});
    const auto root = log.append(session::UserMessage{.text = "first task"});
    const auto original_leaf = log.append(session::AgentOutput{{provider::TextPart{.text = "first answer"}}});

    auto selected = log.select_leaf(root);
    require(selected.has_value(), "failed to select a known session entry");
    const auto alternate_leaf = log.append(session::UserMessage{.text = "alternate task"});

    require(log.id.value == 7 && log.entries.size() == 3, "branching changed the session identity or deleted entries");
    require(log.find(original_leaf) != nullptr, "branching deleted the original leaf");
    require(log.entries.back().parent_id == root, "alternate branch has the wrong parent");

    auto branch = log.active_branch();
    require(branch.size() == 2 && branch[0]->id == root && branch[1]->id == alternate_leaf,
            "active branch did not follow parent links from root to leaf");

    auto invalid = log.select_leaf(session::EntryId{.value = 99});
    require(!invalid && invalid.error().detail.contains("unknown session entry"), "an unknown branch leaf was accepted");
}

void test_checkpoint_projection() {
    session::Session log({.value = 11});
    log.append(session::UserMessage{.text = "old task"});
    log.append(session::AgentOutput{{provider::TextPart{.text = "old answer"}}});

    session::ContextCheckpoint checkpoint;
    checkpoint.items.push_back(session::ContextInput{{provider::TextPart{.text = "summary"}}});
    checkpoint.items.push_back(session::AgentOutput{{provider::TextPart{.text = "preserved answer"}}});
    const auto checkpoint_id = log.append(std::move(checkpoint));
    const auto recent_id = log.append(session::UserMessage{.text = "continue"});

    auto manifest = context::ContextBuilder{}.build({}, log);
    require(manifest.has_value(), "failed to project a checkpointed session");
    require(log.entries.size() == 4, "checkpoint projection mutated the session log");
    require(manifest->omitted_session_entries == 2 && manifest->session_entries.size() == 2 &&
                manifest->session_entries[0] == checkpoint_id && manifest->session_entries[1] == recent_id,
            "manifest did not expose the checkpoint context boundary");
    require(manifest->provider_history.size() == 3 && manifest->provider_history[0].role == provider::Role::USER &&
                manifest->provider_history[1].role == provider::Role::ASSISTANT &&
                manifest->provider_history[2].role == provider::Role::USER,
            "checkpoint items were lowered with incorrect provider roles");
}

i32 run_all() {
    test_append_only_branching();
    test_checkpoint_projection();
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
