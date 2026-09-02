#include "exec.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lighter/async/io/process.h>
#include <lighter/async/io/watcher.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/timeout.h>
#include <lighter/async/runtime/when.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/codec/json/json.h>
#include <lighter/encoding/utf8.h>

#ifdef _WIN32
#include <windows.h>
#endif

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
constexpr usize k_response_envelope_reserve_bytes = 512;
constexpr usize k_shell_task_buffer_bytes = 1024 * 1024;
constexpr usize k_max_shell_tasks = 32;

struct ExecCommandInput {
    std::string cmd;
    std::optional<std::string> workdir;
    std::optional<i64> yield_time_ms;
    std::optional<usize> max_output_chars;
};

struct WriteStdinInput {
    u64 session_id;
    std::optional<std::string> chars;
    std::optional<bool> eof;
    std::optional<bool> kill;
    std::optional<i64> yield_time_ms;
    std::optional<usize> max_output_chars;
};

#ifdef _WIN32
constexpr i32 k_kill_signal = SIGTERM;
#else
constexpr i32 k_kill_signal = SIGKILL;
#endif

struct ShellTaskResponse {
    std::string receipt;
    std::string payload;
    ToolOutcomeKind kind = ToolOutcomeKind::SUCCEEDED;
};

struct ShellOutputSlice {
    std::string payload;
    u64 dropped_bytes = 0;
    u64 remaining_bytes = 0;
};

struct MutexUnlockGuard {
    explicit MutexUnlockGuard(lighter::Mutex &mutex) : mutex(&mutex) {}
    ~MutexUnlockGuard() { mutex->unlock(); }

    MutexUnlockGuard(const MutexUnlockGuard &) = delete;
    MutexUnlockGuard &operator=(const MutexUnlockGuard &) = delete;

    lighter::Mutex *mutex;
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

usize budgeted_output_limit(usize requested, usize output_budget) { return std::min(requested, output_budget); }

#ifdef _WIN32
/// The user's script travels in this variable and is parsed as its own script
/// block, so statements that must come first (`using`, `param`) stay valid
/// while the console and pipeline encodings are pinned to UTF-8 beforehand.
constexpr std::string_view k_pwsh_command_variable = "LIMINAL_EXEC_CMD";
constexpr std::string_view k_pwsh_driver = "[Console]::InputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
                                           "$OutputEncoding = [System.Text.Encoding]::UTF8; "
                                           "& ([scriptblock]::Create($env:LIMINAL_EXEC_CMD))";

std::string narrow_utf8(const wchar_t *text, int length) {
    if (length <= 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(std::max(size, 0)), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    return out;
}

/// The parent environment plus one variable. libuv only fills in a handful of
/// system variables when an explicit environment is given, so the inherited
/// block has to be copied rather than extended.
std::vector<std::string> environment_with(std::string_view name, std::string_view value) {
    std::vector<std::string> env;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (const wchar_t *entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            auto pair = narrow_utf8(entry, static_cast<int>(wcslen(entry)));
            if (pair.size() > name.size() && pair[name.size()] == '=' &&
                std::ranges::equal(std::string_view(pair).substr(0, name.size()), name, [](char a, char b) {
                    return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
                })) {
                continue;
            }
            env.push_back(std::move(pair));
        }
        FreeEnvironmentStringsW(block);
    }
    env.push_back(std::string(name) + "=" + std::string(value));
    return env;
}
#endif

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

std::string summarize_exec(const ToolOutcome &result) {
    const auto session = field(result.receipt, "session_id").value_or("?");
    const auto status = field(result.receipt, "status").value_or(tool_outcome_is_error(result.kind) ? "failed" : "completed");
    std::string summary = "exec session " + std::string(session) + " · ";
    if (const auto code = field(result.receipt, "exit_code")) {
        summary += "exit " + std::string(*code);
    } else {
        summary += status;
    }
    if (!result.payload.empty()) summary += "\n" + bounded_text(result.payload);
    return summary;
}

} // namespace

struct ShellTaskManager {
    struct ShellTask {
        u64 session_id;
        std::string command;
        std::optional<Process::SpawnResult> child;
        lighter::Mutex interaction;
        lighter::Event changed;
        lighter::Event finished;
        std::string output;
        u64 output_offset = 0;
        u64 delivered_offset = 0;
        u64 version = 0;
        bool running = true;
        bool stdin_closed = false;
        bool killed = false;
        std::optional<Process::ExitStatus> exit_status;
        std::optional<std::string> error;

        void append(std::string_view text) {
            output.append(text);
            if (output.size() > k_shell_task_buffer_bytes) {
                const auto removed = output.size() - k_shell_task_buffer_bytes;
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

    explicit ShellTaskManager(std::filesystem::path working_directory) : working_directory(std::move(working_directory)) {}
    ~ShellTaskManager() {
        for (const auto &[session_id, task] : tasks) {
            std::ignore = session_id;
            if (task->running && task->child) {
                std::ignore = task->child->proc.kill_tree(k_kill_signal);
            }
        }
    }

    Task<ShellTaskResponse, Error> start(ExecCommandInput input, usize output_budget);
    Task<ShellTaskResponse, Error> write_stdin(WriteStdinInput input, usize output_budget);

    std::filesystem::path working_directory;
    u64 next_id = 1;
    std::unordered_map<u64, std::shared_ptr<ShellTask>> tasks;
};

namespace {

Task<i32, lighter::Error> drain(Stream &stream, ShellTaskManager::ShellTask *task) {
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
        if (!sanitized.empty()) task->append(sanitized);
        stream.consume(chunk->size());
    }
    std::string tail;
    sanitizer.finish(tail);
    if (!tail.empty()) task->append(tail);
    co_return 0;
}

Task<> pump_shell_task(std::shared_ptr<ShellTaskManager::ShellTask> task) {
    auto &child = *task->child;
    auto joined = co_await WhenAll(drain(child.stdout_pipe, task.get()), drain(child.stderr_pipe, task.get()), child.proc.wait());
    if (!joined) {
        task->fail("process I/O failed: " + std::string(joined.error().message()));
        co_return;
    }
    auto [stdout_done, stderr_done, status] = *std::move(joined);
    std::ignore = stdout_done;
    std::ignore = stderr_done;
    task->complete(status);
}

Result<std::filesystem::path> resolve_working_directory(const ShellTaskManager &manager, const std::optional<std::string> &requested) {
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

void prune_shell_tasks(ShellTaskManager &manager) {
    for (auto iterator = manager.tasks.begin(); iterator != manager.tasks.end();) {
        const auto &task = *iterator->second;
        const auto end = task.output_offset + task.output.size();
        // A killed session exits like any other and its response delivers
        // output, so the delivery rule alone decides when it can go; reclaiming
        // it earlier would discard bytes the kill response told the model to
        // poll for.
        if (!task.running && task.delivered_offset >= end) {
            iterator = manager.tasks.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

Task<> wait_for_finish(ShellTaskManager::ShellTask &task, std::chrono::milliseconds duration) {
    if (!task.running || duration.count() == 0) co_return;
    auto waited = co_await lighter::with_timeout(task.finished.wait(), duration);
    std::ignore = waited;
}

Task<> wait_for_change(ShellTaskManager::ShellTask &task, u64 version, std::chrono::milliseconds duration) {
    if (!task.running || task.version != version || duration.count() == 0) co_return;
    task.changed.reset();
    if (task.version != version) co_return;
    auto waited = co_await lighter::with_timeout(task.changed.wait(), duration);
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

ShellOutputSlice take_output(ShellTaskManager::ShellTask &task, usize limit) {
    ShellOutputSlice result;
    if (task.delivered_offset < task.output_offset) {
        result.dropped_bytes = task.output_offset - task.delivered_offset;
        task.delivered_offset = task.output_offset;
    }
    const auto available = task.output_offset + task.output.size();
    if (task.delivered_offset >= available) return result;

    const auto index = static_cast<usize>(task.delivered_offset - task.output_offset);
    auto count = std::min(limit, task.output.size() - index);
    count = lighter::encoding::utf8::complete_prefix_len(std::string_view(task.output).substr(index, count));
    result.payload = visible_output(std::string_view(task.output).substr(index, count));
    task.delivered_offset += count;
    result.remaining_bytes = available - task.delivered_offset;
    return result;
}

ShellTaskResponse response_for(ShellTaskManager::ShellTask &task, usize limit) {
    std::string receipt = "session_id: " + std::to_string(task.session_id);
    auto kind = ToolOutcomeKind::SUCCEEDED;
    if (task.running) {
        receipt += "\nstatus: running";
    } else if (task.error) {
        receipt += "\nstatus: failed\nreason: shell_task_failed";
        kind = ToolOutcomeKind::FAILED;
    } else {
        receipt += "\nstatus: exited\nexit_code: " + std::to_string(task.exit_status->status);
        if (task.exit_status->term_signal != 0) {
            receipt += "\nterm_signal: " + std::to_string(task.exit_status->term_signal);
        }
        if (task.exit_status->status != 0 || task.exit_status->term_signal != 0) kind = ToolOutcomeKind::FAILED;
    }
    if (task.killed) receipt += "\nkilled: true";
    auto output = take_output(task, limit);
    if (output.dropped_bytes != 0) receipt += "\noutput_dropped_bytes: " + std::to_string(output.dropped_bytes);
    if (output.remaining_bytes != 0) {
        receipt += "\noutput_remaining_bytes: " + std::to_string(output.remaining_bytes);
        receipt += "\nnext_action: poll_with_write_stdin";
    }
    auto payload = std::move(output.payload);
    if (task.error) {
        if (!payload.empty()) payload += '\n';
        payload += "error: " + *task.error;
    }
    return {.receipt = std::move(receipt), .payload = std::move(payload), .kind = kind};
}

} // namespace

Task<ShellTaskResponse, Error> ShellTaskManager::start(ExecCommandInput input, usize output_budget) {
    if (input.cmd.empty()) co_await fail(Error::tool("cmd cannot be empty"));
    auto duration = co_await or_fail(yield_duration(input.yield_time_ms));
    const auto requested_limit = co_await or_fail(output_limit(input.max_output_chars));
    const auto limit = budgeted_output_limit(requested_limit, output_budget);
    auto directory = co_await or_fail(resolve_working_directory(*this, input.workdir));
    prune_shell_tasks(*this);
    if (tasks.size() >= k_max_shell_tasks) co_await fail(Error::tool("too many live shell tasks"));

#ifdef _WIN32
    // pwsh writes piped output in the console's OEM/ANSI code page, so on a
    // non-English locale every non-ASCII byte would reach the model as U+FFFD
    // after sanitization. Pin both the console encodings and the pipeline
    // encoding to UTF-8 before the user's command runs.
    Process::Options options{
        .file = "pwsh",
        .args = {"pwsh", "-NoProfile", "-NonInteractive", "-Command", std::string(k_pwsh_driver)},
        .env = environment_with(k_pwsh_command_variable, input.cmd),
        .cwd = directory.string(),
        .creation = {.process_group = true, .windows_hide = true},
        .streams = {Process::Stdio::pipe(true, false), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#else
    Process::Options options{
        .file = "/bin/sh",
        .args = {"sh", "-lc", input.cmd},
        .cwd = directory.string(),
        .creation = {.process_group = true},
        .streams = {Process::Stdio::pipe(true, false), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#endif
    auto spawned = Process::spawn(options);
    if (!spawned) co_await fail(Error::tool("failed to spawn command shell: " + std::string(spawned.error().message())));

    auto task = std::make_shared<ShellTask>();
    task->session_id = next_id++;
    task->command = std::move(input.cmd);
    task->child = *std::move(spawned);
    const auto session_id = task->session_id;
    auto *state = task.get();
    tasks.emplace(session_id, task);
    lighter::EventLoop::current().schedule(pump_shell_task(std::move(task)));

    co_await wait_for_finish(*state, duration);
    co_return response_for(*state, limit);
}

Task<ShellTaskResponse, Error> ShellTaskManager::write_stdin(WriteStdinInput input, usize output_budget) {
    auto duration = co_await or_fail(yield_duration(input.yield_time_ms));
    const auto requested_limit = co_await or_fail(output_limit(input.max_output_chars));
    const auto limit = budgeted_output_limit(requested_limit, output_budget);
    const auto found = tasks.find(input.session_id);
    if (found == tasks.end()) co_await fail(Error::tool("unknown exec session: " + std::to_string(input.session_id)));
    const auto task_pointer = found->second;
    // Interactions with different tasks may overlap, but one task's input,
    // output cursor, and lifecycle must be observed in call order.
    co_await task_pointer->interaction.lock();
    MutexUnlockGuard interaction_guard(task_pointer->interaction);
    auto &task = *task_pointer;
    const auto version = task.version;

    if (input.chars && !input.chars->empty()) {
        if (!task.running) co_await fail(Error::tool("exec session has already exited: " + std::to_string(input.session_id)));
        if (task.stdin_closed) co_await fail(Error::tool("exec session stdin is already closed: " + std::to_string(input.session_id)));
        auto written = co_await task.child->stdin_pipe.write(std::span<const char>(input.chars->data(), input.chars->size()));
        if (!written) co_await fail(Error::tool("failed to write exec session input: " + std::string(written.error().message())));
    }
    if (input.eof.value_or(false) && !task.stdin_closed) {
        // Dropping the pipe closes the child's stdin, which is how a program
        // reading to end of input learns that no more is coming.
        task.stdin_closed = true;
        task.child->stdin_pipe = lighter::Pipe{};
    }
    if (input.kill.value_or(false) && task.running) {
        task.killed = true;
        if (auto err = task.child->proc.kill_tree(k_kill_signal)) {
            co_await fail(Error::tool("failed to kill exec session: " + std::string(err.message())));
        }
        co_await wait_for_finish(task, duration);
        co_return response_for(task, limit);
    }
    const auto available = task.output_offset + task.output.size();
    if (task.delivered_offset >= available) co_await wait_for_change(task, version, duration);
    co_return response_for(task, limit);
}

void ShellTaskManagerDeleter::operator()(ShellTaskManager *tasks) const { delete tasks; }

ShellTaskManagerPtr make_shell_task_manager(std::filesystem::path working_directory) {
    return ShellTaskManagerPtr(new ShellTaskManager(std::move(working_directory)));
}

std::array<ToolRegistration, 2> make_exec_tools(ShellTaskManager &tasks) {
    auto exec = ToolRegistration{
        .definition =
            {
                .name = "exec_command",
#ifdef _WIN32
                .description = "Run a PowerShell command. Returns an exit status, or a session ID when still running. Use "
                               "write_stdin to send input or poll that session. Prefer ripgrep (rg) for text search and uutils commands "
                               "for ordinary filesystem work.",
#else
                .description = "Run a POSIX shell command. Returns an exit status, or a session ID when still running. Use "
                               "write_stdin to send input or poll that session. Prefer ripgrep (rg) for text search and uutils commands "
                               "for ordinary filesystem work.",
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
        .execution_mode = ToolExecutionMode::PARALLEL,
        .receipt_bytes = k_response_envelope_reserve_bytes,
        .validate = [](const provider::ToolCall &call) -> Result<void> {
            auto input = parse_input<ExecCommandInput>(call.input);
            if (!input) return lighter::outcome_error(std::move(input).error());
            return {};
        },
        .execute = [&tasks](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant grant) -> Task<ToolOutcome, Error> {
            auto input = co_await or_fail(parse_input<ExecCommandInput>(call.input));
            auto response = co_await tasks.start(std::move(input), grant.payload_bytes).or_fail();
            co_return ToolOutcome{
                .call_id = call.id, .kind = response.kind, .receipt = std::move(response.receipt), .payload = std::move(response.payload)};
        },
        .describe =
            [](const provider::ToolCall &call) {
                const auto input = parse_input<ExecCommandInput>(call.input);
                return input ? ToolCallPresentation{.command = bounded_text(input->cmd)} :
                               ToolCallPresentation{.description = "Run command"};
            },
        .summarize = [](const provider::ToolCall &, const ToolOutcome &result) { return summarize_exec(result); },
    };

    auto write_stdin = ToolRegistration{
        .definition =
            {
                .name = "write_stdin",
                .description = "Write characters to an existing exec session and return incremental output. Empty or omitted chars "
                               "polls only. Set eof to close the session's input after writing, or kill to terminate the session "
                               "and every process it started.",
                .input_schema =
                    {
                        .properties =
                            {
                                {"session_id", {.type = "integer", .description = "Session ID returned by exec_command."}},
                                {"chars", {.type = "string", .description = "Characters to write. Empty or omitted means poll only."}},
                                {"eof", {.type = "boolean", .description = "Close the session's stdin after writing chars."}},
                                {"kill", {.type = "boolean", .description = "Terminate the session and its whole process tree."}},
                                {"yield_time_ms", {.type = "integer", .description = "Wait up to 30000 ms for new output; default 10000."}},
                                {"max_output_chars",
                                 {.type = "integer", .description = "Maximum output bytes returned now; 1024-131072, default 32768."}},
                            },
                        .required = {"session_id"},
                    },
            },
        .execution_mode = ToolExecutionMode::PARALLEL,
        .receipt_bytes = k_response_envelope_reserve_bytes,
        .validate = [](const provider::ToolCall &call) -> Result<void> {
            auto input = parse_input<WriteStdinInput>(call.input);
            if (!input) return lighter::outcome_error(std::move(input).error());
            return {};
        },
        .execute = [&tasks](const ToolSet &, const provider::ToolCall &call, ToolOutputGrant grant) -> Task<ToolOutcome, Error> {
            auto input = co_await or_fail(parse_input<WriteStdinInput>(call.input));
            auto response = co_await tasks.write_stdin(std::move(input), grant.payload_bytes).or_fail();
            co_return ToolOutcome{
                .call_id = call.id, .kind = response.kind, .receipt = std::move(response.receipt), .payload = std::move(response.payload)};
        },
        .describe =
            [](const provider::ToolCall &call) {
                const auto input = parse_input<WriteStdinInput>(call.input);
                if (!input) return ToolCallPresentation{.description = "Write to exec session"};
                const auto writes = input->chars && !input->chars->empty();
                const auto action = input->kill.value_or(false) ? "Kill exec session " :
                                    writes                      ? "Write to exec session " :
                                    input->eof.value_or(false)  ? "Close stdin of exec session " :
                                                                  "Poll exec session ";
                return ToolCallPresentation{.description = action + std::to_string(input->session_id)};
            },
        .summarize = [](const provider::ToolCall &, const ToolOutcome &result) { return summarize_exec(result); },
    };
    return {std::move(exec), std::move(write_stdin)};
}

} // namespace liminal
