#include "exec.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <lighter/async/io/process.h>
#include <lighter/async/io/watcher.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/timeout.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/codec/json/json.h>
#include <lighter/encoding/utf8.h>

namespace liminal {

namespace json = lighter::codec::json;
using lighter::fail;
using lighter::or_fail;
using lighter::outcome_error;
using lighter::Process;
using lighter::Stream;
using lighter::Task;
using lighter::WhenAll;

namespace {

constexpr i64 k_default_yield_ms = 10'000;
constexpr i64 k_max_yield_ms = 30'000;
constexpr usize k_default_output_chars = 32 * 1024;
constexpr usize k_min_output_chars = 1024;
constexpr usize k_max_output_chars = 128 * 1024;
constexpr usize k_session_buffer_bytes = 1024 * 1024;
constexpr usize k_max_sessions = 32;

struct ExecCommandInput {
    std::string cmd;
    std::optional<std::string> workdir;
    std::optional<i64> yield_time_ms;
    std::optional<usize> max_output_chars;
};

struct WriteStdinInput {
    std::string session_id;
    std::optional<std::string> chars;
    std::optional<i64> yield_time_ms;
    std::optional<usize> max_output_chars;
};

struct ExecResponse {
    std::string content;
    bool is_error = false;
};

template <typename T>
Result<T> parse_input(const glz::generic &input) {
    auto encoded = json::to_string(input);
    if (!encoded) return outcome_error(Error::json(std::move(encoded).error(), "tool input re-encode"));
    auto parsed = json::parse<T>(*encoded);
    if (!parsed) return outcome_error(Error::json(std::move(parsed).error(), "tool input"));
    return *std::move(parsed);
}

Result<std::chrono::milliseconds> yield_duration(std::optional<i64> requested) {
    const auto value = requested.value_or(k_default_yield_ms);
    if (value < 0 || value > k_max_yield_ms) {
        return outcome_error(Error::tool("yield_time_ms must be between 0 and 30000"));
    }
    return std::chrono::milliseconds(value);
}

Result<usize> output_limit(std::optional<usize> requested) {
    const auto value = requested.value_or(k_default_output_chars);
    if (value < k_min_output_chars || value > k_max_output_chars) {
        return outcome_error(Error::tool("max_output_chars must be between 1024 and 131072"));
    }
    return value;
}

std::string bounded_text(std::string_view text, usize limit = 2 * 1024) {
    if (text.size() <= limit) return std::string(text);
    auto end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + "…";
}

std::optional<std::string_view> field(std::string_view content, std::string_view name) {
    const auto marker = std::string(name) + ": ";
    const auto start = content.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value = start + marker.size();
    const auto end = content.find('\n', value);
    return content.substr(value, end == std::string_view::npos ? content.size() - value : end - value);
}

std::string summarize_exec(const provider::ToolResult &result) {
    const auto session = field(result.content, "session_id").value_or("?");
    const auto status = field(result.content, "status").value_or(result.is_error ? "failed" : "completed");
    std::string summary = "session " + std::string(session) + " · ";
    if (const auto code = field(result.content, "exit_code")) {
        summary += "exit " + std::string(*code);
    } else {
        summary += status;
    }
    constexpr std::string_view marker = "\n\noutput:\n";
    if (const auto output = result.content.find(marker); output != std::string_view::npos) {
        summary += "\n" + bounded_text(std::string_view(result.content).substr(output + marker.size()));
    }
    return summary;
}

} // namespace

struct ExecSessionManager {
    struct Session {
        std::string id;
        std::string command;
        std::optional<Process::SpawnResult> child;
        lighter::Event changed;
        lighter::Event finished;
        std::string output;
        u64 output_offset = 0;
        u64 delivered_offset = 0;
        u64 version = 0;
        bool running = true;
        std::optional<Process::ExitStatus> exit_status;
        std::optional<std::string> error;

        void append(std::string_view text) {
            output.append(text);
            if (output.size() > k_session_buffer_bytes) {
                const auto removed = output.size() - k_session_buffer_bytes;
                output.erase(0, removed);
                output_offset += removed;
            }
            ++version;
            changed.set();
        }

        void complete(Process::ExitStatus status) {
            running = false;
            exit_status = status;
            ++version;
            changed.set();
            finished.set();
        }

        void fail(std::string message) {
            running = false;
            error = std::move(message);
            ++version;
            changed.set();
            finished.set();
        }
    };

    explicit ExecSessionManager(std::filesystem::path working_directory) : working_directory(std::move(working_directory)) {}
    ~ExecSessionManager() {
        for (const auto &[id, session] : sessions) {
            std::ignore = id;
            if (session->running && session->child) {
#ifdef _WIN32
                std::ignore = session->child->proc.kill(SIGTERM);
#else
                std::ignore = session->child->proc.kill(SIGKILL);
#endif
            }
        }
    }

    Task<ExecResponse, Error> start(ExecCommandInput input);
    Task<ExecResponse, Error> write(WriteStdinInput input);

    std::filesystem::path working_directory;
    u64 next_id = 1;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
};

namespace {

Task<i32, lighter::Error> drain(Stream &stream, ExecSessionManager::Session *session) {
    lighter::encoding::utf8::Sanitizer sanitizer;
    while (true) {
        auto chunk = co_await stream.read_chunk();
        if (!chunk) {
            if (chunk.error() == lighter::Error::k_end_of_file) break;
            co_await fail(std::move(chunk).error());
        }
        if (chunk->empty()) break;
        std::string sanitized;
        sanitizer.feed(std::string_view(chunk->data(), chunk->size()), sanitized);
        if (!sanitized.empty()) session->append(sanitized);
        stream.consume(chunk->size());
    }
    std::string tail;
    sanitizer.finish(tail);
    if (!tail.empty()) session->append(tail);
    co_return 0;
}

Task<> pump_session(std::shared_ptr<ExecSessionManager::Session> session) {
    auto &child = *session->child;
    auto joined = co_await WhenAll(drain(child.stdout_pipe, session.get()), drain(child.stderr_pipe, session.get()), child.proc.wait());
    if (!joined) {
        session->fail("process I/O failed: " + std::string(joined.error().message()));
        co_return;
    }
    auto [stdout_done, stderr_done, status] = *std::move(joined);
    std::ignore = stdout_done;
    std::ignore = stderr_done;
    session->complete(status);
}

Result<std::filesystem::path> resolve_working_directory(const ExecSessionManager &manager, const std::optional<std::string> &requested) {
    auto path = requested ? std::filesystem::path(*requested) : manager.working_directory;
    if (path.is_relative()) path = manager.working_directory / path;
    std::error_code error;
    path = std::filesystem::canonical(path, error);
    if (error) return outcome_error(Error::tool("cannot resolve workdir: " + error.message()));
    if (!std::filesystem::is_directory(path, error) || error) {
        return outcome_error(Error::tool("workdir is not a directory: '" + path.string() + "'"));
    }
    return path;
}

void prune_sessions(ExecSessionManager &manager) {
    for (auto iterator = manager.sessions.begin(); iterator != manager.sessions.end();) {
        const auto &session = *iterator->second;
        const auto end = session.output_offset + session.output.size();
        if (!session.running && session.delivered_offset >= end) {
            iterator = manager.sessions.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

Task<> wait_for_finish(ExecSessionManager::Session &session, std::chrono::milliseconds duration) {
    if (!session.running || duration.count() == 0) co_return;
    auto waited = co_await lighter::with_timeout(session.finished.wait(), duration);
    std::ignore = waited;
}

Task<> wait_for_change(ExecSessionManager::Session &session, u64 version, std::chrono::milliseconds duration) {
    if (!session.running || session.version != version || duration.count() == 0) co_return;
    session.changed.reset();
    if (session.version != version) co_return;
    auto waited = co_await lighter::with_timeout(session.changed.wait(), duration);
    std::ignore = waited;
}

std::string visible_output(std::string_view raw) {
    std::string result;
    result.reserve(raw.size());
    for (usize index = 0; index < raw.size(); ++index) {
        const auto byte = static_cast<unsigned char>(raw[index]);
        if (byte == 0x1b) {
            if (index + 1 < raw.size() && raw[index + 1] == '[') {
                index += 2;
                while (index < raw.size()) {
                    const auto final = static_cast<unsigned char>(raw[index]);
                    if (final >= 0x40 && final <= 0x7e) break;
                    ++index;
                }
            } else if (index + 1 < raw.size()) {
                ++index;
            }
            continue;
        }
        if (byte < 0x20 && raw[index] != '\n' && raw[index] != '\r' && raw[index] != '\t') continue;
        if (byte == 0x7f) continue;
        result += raw[index];
    }
    return result;
}

std::string take_output(ExecSessionManager::Session &session, usize limit) {
    std::string result;
    if (session.delivered_offset < session.output_offset) {
        result = "[" + std::to_string(session.output_offset - session.delivered_offset) + " earlier bytes were truncated]\n";
        session.delivered_offset = session.output_offset;
    }
    const auto available = session.output_offset + session.output.size();
    if (session.delivered_offset >= available) return result;

    const auto index = static_cast<usize>(session.delivered_offset - session.output_offset);
    auto count = std::min(limit, session.output.size() - index);
    count = lighter::encoding::utf8::complete_prefix_len(std::string_view(session.output).substr(index, count));
    result += visible_output(std::string_view(session.output).substr(index, count));
    session.delivered_offset += count;
    if (session.delivered_offset < available) {
        result += "\n[" + std::to_string(available - session.delivered_offset) + " bytes remain; poll with write_stdin and empty chars]\n";
    }
    return result;
}

ExecResponse response_for(ExecSessionManager::Session &session, usize limit) {
    std::string result = "session_id: " + session.id;
    bool is_error = false;
    if (session.running) {
        result += "\nstatus: running";
    } else if (session.error) {
        result += "\nstatus: failed\nerror: " + *session.error;
        is_error = true;
    } else {
        result += "\nstatus: exited\nexit_code: " + std::to_string(session.exit_status->status);
        if (session.exit_status->term_signal != 0) {
            result += "\nterm_signal: " + std::to_string(session.exit_status->term_signal);
        }
        is_error = session.exit_status->status != 0 || session.exit_status->term_signal != 0;
    }
    auto output = take_output(session, limit);
    if (!output.empty()) result += "\n\noutput:\n" + output;
    return {.content = std::move(result), .is_error = is_error};
}

} // namespace

Task<ExecResponse, Error> ExecSessionManager::start(ExecCommandInput input) {
    if (input.cmd.empty()) co_await fail(Error::tool("cmd cannot be empty"));
    auto duration = co_await or_fail(yield_duration(input.yield_time_ms));
    const auto limit = co_await or_fail(output_limit(input.max_output_chars));
    auto directory = co_await or_fail(resolve_working_directory(*this, input.workdir));
    prune_sessions(*this);
    if (sessions.size() >= k_max_sessions) co_await fail(Error::tool("too many live exec sessions"));

#ifdef _WIN32
    Process::Options options{
        .file = "pwsh",
        .args = {"pwsh", "-NoProfile", "-NonInteractive", "-Command", input.cmd},
        .cwd = directory.string(),
        .creation = {.windows_hide = true},
        .streams = {Process::Stdio::pipe(true, false), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#else
    Process::Options options{
        .file = "/bin/sh",
        .args = {"sh", "-lc", input.cmd},
        .cwd = directory.string(),
        .streams = {Process::Stdio::pipe(true, false), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#endif
    auto spawned = Process::spawn(options);
    if (!spawned) co_await fail(Error::tool("failed to spawn command shell: " + std::string(spawned.error().message())));

    auto session = std::make_shared<Session>();
    session->id = std::to_string(next_id++);
    session->command = std::move(input.cmd);
    session->child = *std::move(spawned);
    const auto id = session->id;
    auto *state = session.get();
    sessions.emplace(id, session);
    lighter::EventLoop::current().schedule(pump_session(std::move(session)));

    co_await wait_for_finish(*state, duration);
    co_return response_for(*state, limit);
}

Task<ExecResponse, Error> ExecSessionManager::write(WriteStdinInput input) {
    auto duration = co_await or_fail(yield_duration(input.yield_time_ms));
    const auto limit = co_await or_fail(output_limit(input.max_output_chars));
    const auto found = sessions.find(input.session_id);
    if (found == sessions.end()) co_await fail(Error::tool("unknown exec session: " + input.session_id));
    auto &session = *found->second;
    const auto version = session.version;

    if (input.chars && !input.chars->empty()) {
        if (!session.running) co_await fail(Error::tool("exec session has already exited: " + input.session_id));
        auto written = co_await session.child->stdin_pipe.write(std::span<const char>(input.chars->data(), input.chars->size()));
        if (!written) co_await fail(Error::tool("failed to write exec session input: " + std::string(written.error().message())));
    }
    const auto available = session.output_offset + session.output.size();
    if (session.delivered_offset >= available) co_await wait_for_change(session, version, duration);
    co_return response_for(session, limit);
}

void ExecSessionManagerDeleter::operator()(ExecSessionManager *sessions) const { delete sessions; }

ExecSessionManagerPtr make_exec_session_manager(std::filesystem::path working_directory) {
    return ExecSessionManagerPtr(new ExecSessionManager(std::move(working_directory)));
}

std::array<ToolRegistration, 2> make_exec_tools(ExecSessionManager &sessions) {
    auto exec = ToolRegistration{
        .definition =
            {
                .name = "exec_command",
#ifdef _WIN32
                .description =
                    "Run a PowerShell command. Returns an exit status, or a session ID when still running. Use write_stdin to send "
                    "input or poll that session. Prefer ripgrep (rg) for text search and uutils commands for ordinary filesystem work.",
#else
                .description =
                    "Run a POSIX shell command. Returns an exit status, or a session ID when still running. Use write_stdin to send "
                    "input or poll that session. Prefer ripgrep (rg) for text search and uutils commands for ordinary filesystem work.",
#endif
                .input_schema =
                    {
                        .properties =
                            {
                                {"cmd", {.type = "string", .description = "Shell command text."}},
                                {"workdir", {.type = "string", .description = "Absolute or working-directory-relative directory."}},
                                {"yield_time_ms", {.type = "integer", .description = "Wait up to 30000 ms; defaults to 10000."}},
                                {"max_output_chars",
                                 {.type = "integer", .description = "Maximum output bytes returned now; 1024-131072, default 32768."}},
                            },
                        .required = {"cmd"},
                    },
            },
        .execute = [&sessions](const ToolSet &, const provider::ToolCall &call) -> Task<provider::ToolResult, Error> {
            auto input = co_await or_fail(parse_input<ExecCommandInput>(call.input));
            auto response = co_await sessions.start(std::move(input)).or_fail();
            co_return provider::ToolResult{.call_id = call.id, .content = std::move(response.content), .is_error = response.is_error};
        },
        .describe =
            [](const provider::ToolCall &call) {
                const auto input = parse_input<ExecCommandInput>(call.input);
                return input ? ToolCallPresentation{.command = bounded_text(input->cmd)} :
                               ToolCallPresentation{.description = "Run command"};
            },
        .summarize = [](const provider::ToolCall &, const provider::ToolResult &result) { return summarize_exec(result); },
    };

    auto write = ToolRegistration{
        .definition =
            {
                .name = "write_stdin",
                .description = "Write characters to a running exec_command session, or poll it by passing an empty chars string.",
                .input_schema =
                    {
                        .properties =
                            {
                                {"session_id", {.type = "string", .description = "Session ID returned by exec_command."}},
                                {"chars", {.type = "string", .description = "Characters to write. Empty or omitted means poll only."}},
                                {"yield_time_ms", {.type = "integer", .description = "Wait up to 30000 ms for new output; default 10000."}},
                                {"max_output_chars",
                                 {.type = "integer", .description = "Maximum output bytes returned now; 1024-131072, default 32768."}},
                            },
                        .required = {"session_id"},
                    },
            },
        .execute = [&sessions](const ToolSet &, const provider::ToolCall &call) -> Task<provider::ToolResult, Error> {
            auto input = co_await or_fail(parse_input<WriteStdinInput>(call.input));
            auto response = co_await sessions.write(std::move(input)).or_fail();
            co_return provider::ToolResult{.call_id = call.id, .content = std::move(response.content), .is_error = response.is_error};
        },
        .describe =
            [](const provider::ToolCall &call) {
                const auto input = parse_input<WriteStdinInput>(call.input);
                return ToolCallPresentation{.description =
                                                input ? "Write stdin for session " + bounded_text(input->session_id) : "Write stdin"};
            },
        .summarize = [](const provider::ToolCall &, const provider::ToolResult &result) { return summarize_exec(result); },
    };
    return {std::move(exec), std::move(write)};
}

} // namespace liminal
