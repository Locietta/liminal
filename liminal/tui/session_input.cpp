#include "session_input.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

#include <liminal/tui/command.h>

namespace liminal::tui {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string escaped_prompt(std::string text) {
    if (text.starts_with('/')) text.insert(text.begin(), '/');
    return text;
}

} // namespace

lighter::Error SessionInput::refresh() {
    std::vector<std::string> rows;
    const auto active = agent->active_task();
    for (const auto &input : agent->session.pending_steering()) {
        const auto state = active && active->task_id == input.task_id ? "Steer s" : "Unsent s";
        rows.push_back(state + std::to_string(input.id.value) + ": " + sanitize_terminal_text(input.text, false));
    }
    for (const auto &input : queued) {
        if (input.session_id != agent->session.id) continue;
        rows.push_back(std::string(input.held ? "Unsent q" : "Queued q") + std::to_string(input.id) + ": " +
                       sanitize_terminal_text(input.text, false));
    }
    renderer->screen.set_pending_inputs(std::move(rows));
    return renderer->redraw();
}

lighter::Error SessionInput::manage_pending(std::string_view arguments) {
    arguments = trim(arguments);
    if (arguments.empty()) {
        if (auto error = refresh()) return error;
        std::string listing;
        for (const auto &row : renderer->screen.pending_inputs) listing += row + '\n';
        if (listing.empty()) listing = "No pending input.\n";
        listing += "/pending edit <id> returns input to the composer; /pending drop <id> removes it.\n";
        return renderer->notice(listing);
    }
    const auto separator = arguments.find_first_of(" \t");
    const auto action = arguments.substr(0, separator);
    auto identifier = separator == std::string_view::npos ? std::string_view{} : trim(arguments.substr(separator));
    if ((action != "edit" && action != "drop") || identifier.size() < 2 || (identifier.front() != 's' && identifier.front() != 'q')) {
        return renderer->notice("[usage: /pending [edit|drop <sN|qN>]]\n");
    }
    u64 id = 0;
    const auto number = identifier.substr(1);
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), id);
    if (id == 0 || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
        return renderer->notice("[usage: /pending [edit|drop <sN|qN>]]\n");
    }
    if (action == "edit" && !renderer->prompt_empty()) return renderer->notice("[clear the current draft before editing pending input]\n");
    std::string text;
    if (identifier.front() == 's') {
        auto withdrawn = agent->session.withdraw_steering({.value = id});
        if (!withdrawn) return renderer->notice("[" + withdrawn.error().message() + "]\n");
        text = *std::move(withdrawn);
        text = escaped_prompt(std::move(text));
    } else {
        const auto found =
            std::ranges::find_if(queued, [&](const auto &input) { return input.id == id && input.session_id == agent->session.id; });
        if (found == queued.end()) return renderer->notice("[input is no longer pending]\n");
        text = std::move(found->text);
        queued.erase(found);
    }
    if (action == "edit") {
        if (auto error = renderer->replace_prompt(std::move(text))) return error;
    }
    return refresh();
}

lighter::Error SessionInput::submit(std::string text) {
    if (text.empty()) return renderer->redraw();
    auto parsed = parse_repl_input(text);
    if (!parsed) return renderer->notice("[" + parsed.error().message() + "]\n");
    if (const auto *command = std::get_if<CommandLine>(&*parsed)) {
        const auto *spec = find_command(command->name);
        if (!spec) return renderer->notice("[unknown command '/" + command->name + "']\n");
        if (spec->kind == CommandKind::PENDING) return manage_pending(command->arguments);
        if (spec->kind == CommandKind::QUEUE) {
            if (command->arguments.empty()) return renderer->notice("[usage: /queue <prompt>]\n");
            queued.push_back({.id = next_id++, .session_id = agent->session.id, .text = escaped_prompt(command->arguments)});
        } else {
            if (spec->idle_only && agent->active_task()) {
                return renderer->notice("[/" + command->name + " is available only while the agent is idle]\n");
            }
            queued.push_back({.id = next_id++, .session_id = agent->session.id, .text = std::move(text), .command = true});
        }
    } else if (const auto active = agent->active_task()) {
        auto accepted = agent->steer(*active, std::move(std::get<UserPrompt>(*parsed).text));
        if (!accepted) {
            if (auto error = renderer->replace_prompt(std::move(text))) return error;
            return renderer->notice("[" + accepted.error().message() + "]\n");
        }
    } else {
        queued.push_back({.id = next_id++, .session_id = agent->session.id, .text = std::move(text)});
    }
    ready.set();
    return refresh();
}

lighter::Task<std::string> SessionInput::next() {
    while (true) {
        const auto found =
            std::ranges::find_if(queued, [&](const auto &input) { return input.session_id == agent->session.id && !input.held; });
        if (found != queued.end()) {
            auto text = std::move(found->text);
            queued.erase(found);
            co_return text;
        }
        ready.reset();
        co_await ready.wait();
    }
}

void SessionInput::pause_follow_ups() {
    for (auto &input : queued)
        if (input.session_id == agent->session.id && !input.command) input.held = true;
}

} // namespace liminal::tui
