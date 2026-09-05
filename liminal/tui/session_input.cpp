#include "session_input.h"

#include <charconv>
#include <string_view>
#include <utility>
#include <vector>

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
    if ((action != "edit" && action != "drop") || identifier.size() < 2 || identifier.front() != 's') {
        return renderer->notice("[usage: /pending [edit|drop <sN>]]\n");
    }
    u64 id = 0;
    const auto number = identifier.substr(1);
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), id);
    if (id == 0 || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
        return renderer->notice("[usage: /pending [edit|drop <sN>]]\n");
    }
    if (action == "edit" && !renderer->prompt_empty()) return renderer->notice("[clear the current draft before editing pending input]\n");
    auto withdrawn = agent->session.withdraw_steering({.value = id});
    if (!withdrawn) return renderer->notice("[" + withdrawn.error().message() + "]\n");
    if (action == "edit") {
        if (auto error = renderer->replace_prompt(escaped_prompt(*std::move(withdrawn)))) return error;
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
        if (spec->idle_only && agent->active_task()) {
            return renderer->notice("[/" + command->name + " is available only while the agent is idle]\n");
        }
    } else if (const auto active = agent->active_task()) {
        auto accepted = agent->steer(*active, std::move(std::get<UserPrompt>(*parsed).text));
        if (!accepted) {
            if (auto error = renderer->replace_prompt(std::move(text))) return error;
            return renderer->notice("[" + accepted.error().message() + "]\n");
        }
        return refresh();
    }
    submissions.push_back(std::move(text));
    ready.set();
    return renderer->redraw();
}

lighter::Task<std::string> SessionInput::next() {
    while (submissions.empty()) {
        ready.reset();
        co_await ready.wait();
    }
    auto text = std::move(submissions.front());
    submissions.pop_front();
    co_return text;
}

} // namespace liminal::tui
