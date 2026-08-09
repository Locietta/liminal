#include "tools.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/io/fs.h>
#include <lighter/async/io/process.h>
#include <lighter/async/runtime/timeout.h>
#include <lighter/async/runtime/when.h>
#include <lighter/codec/json/json.h>

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

constexpr usize k_file_limit = 128 * 1024;
constexpr usize k_capture_head = 32 * 1024;
constexpr usize k_capture_tail = 32 * 1024;
constexpr i64 k_min_command_timeout_ms = 1;
constexpr i64 k_max_command_timeout_ms = 60 * 60 * 1000;
constexpr usize k_max_parallel_call_limit = 32;
constexpr usize k_max_turn_call_limit = 256;
constexpr usize k_max_call_summary_bytes = 2 * 1024;
constexpr usize k_max_preview_line_bytes = 240;
constexpr usize k_max_preview_lines = 4;

struct ReadFileInput {
    std::string path;
};

struct RunCommandInput {
    std::string command;
};

std::string bounded_text(std::string_view text, usize limit) {
    if (text.size() <= limit) return std::string(text);
    auto end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + "…";
}

std::string normalize_newlines(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (usize index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
            result += '\n';
        } else {
            result += text[index];
        }
    }
    return result;
}

std::vector<std::string> content_lines(std::string_view text) {
    std::vector<std::string> result;
    usize start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find_first_not_of(" \t") != std::string_view::npos) {
            result.push_back(bounded_text(line, k_max_preview_line_bytes));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::vector<std::string> preview_lines(const std::vector<std::string> &lines) {
    if (lines.size() <= k_max_preview_lines) return lines;
    return {
        lines[0],     lines[1], "… " + std::to_string(lines.size() - k_max_preview_lines) + " lines omitted …", lines[lines.size() - 2],
        lines.back(),
    };
}

std::string append_preview(std::string summary, std::string_view label, const std::vector<std::string> &lines) {
    if (lines.empty()) return summary;
    summary += "\n";
    summary += label;
    summary += ":";
    for (const auto &line : preview_lines(lines)) summary += "\n" + line;
    return summary;
}

std::string line_count(usize count, std::string_view stream = {}) {
    std::string result;
    if (!stream.empty()) result = std::string(stream) + " ";
    result += std::to_string(count) + (count == 1 ? " line" : " lines");
    return result;
}

std::string byte_count(usize bytes) {
    if (bytes < 1024) return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
    constexpr usize kibibyte = 1024;
    constexpr usize mebibyte = 1024 * kibibyte;
    const auto unit = bytes < mebibyte ? kibibyte : mebibyte;
    const auto tenths = (bytes * 10 + unit / 2) / unit;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + (unit == kibibyte ? " KiB" : " MiB");
}

std::string generic_result_summary(const provider::ToolResult &result) {
    const auto lines = content_lines(result.content);
    if (lines.empty()) return result.is_error ? "No error detail" : "No output";
    std::string summary = result.is_error ? "Error" : line_count(lines.size());
    for (const auto &line : preview_lines(lines)) summary += "\n" + line;
    return summary;
}

std::string command_result_summary(const provider::ToolResult &result) {
    constexpr std::string_view stdout_marker = "\n\nstdout:\n";
    constexpr std::string_view stderr_marker = "\n\nstderr:\n";
    const auto stdout_at = result.content.find(stdout_marker);
    const auto stderr_at = result.content.find(stderr_marker);
    if (stdout_at == std::string_view::npos || stderr_at == std::string_view::npos || stderr_at < stdout_at) {
        return generic_result_summary(result);
    }

    const auto header = std::string_view(result.content).substr(0, stdout_at);
    const auto stdout_text =
        std::string_view(result.content).substr(stdout_at + stdout_marker.size(), stderr_at - stdout_at - stdout_marker.size());
    const auto stderr_text = std::string_view(result.content).substr(stderr_at + stderr_marker.size());
    const auto stdout_lines = content_lines(stdout_text);
    const auto stderr_lines = content_lines(stderr_text);

    std::string summary;
    const auto header_lines = content_lines(header);
    if (!header_lines.empty() && header_lines.front().starts_with("exit_code: ")) {
        summary = "exit " + header_lines.front().substr(std::string_view("exit_code: ").size());
    } else {
        summary = result.is_error ? "failed" : "completed";
    }
    if (header_lines.size() > 1 && header_lines[1].starts_with("term_signal: ")) {
        summary += " · signal " + header_lines[1].substr(std::string_view("term_signal: ").size());
    }
    if (stdout_lines.empty() && stderr_lines.empty()) {
        summary += " · no output";
    } else {
        if (!stdout_lines.empty()) summary += " · " + line_count(stdout_lines.size(), "stdout");
        if (!stderr_lines.empty()) summary += " · " + line_count(stderr_lines.size(), "stderr");
    }
    summary = append_preview(std::move(summary), "stdout", stdout_lines);
    return append_preview(std::move(summary), "stderr", stderr_lines);
}

/// Decode a tool_use input (already validated as a JSON object) into the
/// tool's typed input struct. Strict: unknown keys are rejected here, matching
/// the additionalProperties:false schema we advertise.
template <typename T>
Result<T> parse_input(const glz::generic &input) {
    auto encoded = json::to_string(input);
    if (!encoded) {
        return outcome_error(Error::json(std::move(encoded).error(), "tool input re-encode"));
    }
    auto typed = json::parse<T>(*encoded);
    if (!typed) {
        return outcome_error(Error::json(std::move(typed).error(), "tool input"));
    }
    return *std::move(typed);
}

template <typename T>
Result<T> parse_bounded_environment(std::string_view name, T fallback, T minimum, T maximum) {
    const auto variable = std::string(name);
    const char *raw = std::getenv(variable.c_str());
    if (!raw || !*raw) {
        return fallback;
    }

    T value{};
    const std::string_view text(raw);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum) {
        return outcome_error(
            Error::config(variable + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum)));
    }
    return value;
}

Result<std::filesystem::path> resolve_read_path(const ToolSet &tools, const ReadFileInput &input) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(tools.working_directory, error);
    if (error) {
        return outcome_error(Error::tool("cannot resolve workspace: " + error.message()));
    }

    auto requested = std::filesystem::path(input.path);
    if (requested.is_relative()) requested = root / requested;
    auto resolved = std::filesystem::weakly_canonical(requested, error);
    if (error) {
        return outcome_error(Error::tool("cannot resolve '" + requested.string() + "': " + error.message()));
    }
    return resolved;
}

std::string tool_read_file(const ToolSet &tools, const ReadFileInput &input) {
    auto resolved = resolve_read_path(tools, input);
    if (!resolved) return "Error: " + resolved.error().message();
    const auto path = resolved->string();

    std::error_code status_error;
    const auto status = std::filesystem::status(*resolved, status_error);
    if (status_error) {
        return "Error: cannot inspect '" + path + "': " + status_error.message();
    }
    if (!std::filesystem::is_regular_file(status)) {
        return "Error: '" + path + "' is not a regular file";
    }

    auto content = lighter::fs::sync::read_to_string(path, k_file_limit + 1);
    if (!content) {
        return "Error: cannot read '" + path + "': " + std::string(content.error().message());
    }
    if (content->find('\0') != std::string::npos) {
        return "Error: '" + path + "' looks like a binary file";
    }
    if (content->size() > k_file_limit) {
        content->resize(k_file_limit);
        *content += "\n... [truncated after " + std::to_string(k_file_limit) + " bytes]";
    }
    return *std::move(content);
}

/// Bounded head/tail capture: keeps the first/last N bytes but always drains
/// the pipe fully so the child never blocks on a full pipe buffer.
struct BoundedCapture {
    std::string head;
    std::string tail;
    usize dropped = 0;

    void append(std::string_view data) {
        if (head.size() < k_capture_head) {
            auto take = std::min(data.size(), k_capture_head - head.size());
            head.append(data.data(), take);
            data.remove_prefix(take);
        }
        if (data.empty()) {
            return;
        }
        tail.append(data);
        if (tail.size() > k_capture_tail) {
            auto excess = tail.size() - k_capture_tail;
            dropped += excess;
            tail.erase(0, excess);
        }
    }

    std::string text() && {
        if (dropped == 0 && tail.empty()) {
            return sanitize(std::move(head));
        }
        return sanitize(std::move(head) + "\n... [" + std::to_string(dropped) + " bytes omitted] ...\n" + tail);
    }

private:
    /// Command output is arbitrary bytes; ANSI styling and raw control
    /// characters are noise to the model (and unescaped control bytes are
    /// not even valid inside JSON strings). Strip both.
    static std::string sanitize(std::string raw) {
        std::string out;
        out.reserve(raw.size());
        for (usize i = 0; i < raw.size(); ++i) {
            unsigned char c = raw[i];
            bool control = c < 0x20 && c != '\n' && c != '\r' && c != '\t';
            if (control) {
                // A control byte introducing '[' is (mangled) CSI; skip the
                // whole sequence through its final byte in [0x40, 0x7e].
                if (i + 1 < raw.size() && raw[i + 1] == '[') {
                    i += 2;
                    while (i < raw.size() && (static_cast<unsigned char>(raw[i]) < 0x40 || static_cast<unsigned char>(raw[i]) > 0x7e)) {
                        ++i;
                    }
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
        }
        return out;
    }
};

Task<BoundedCapture, lighter::Error> drain(Stream &stream) {
    BoundedCapture capture;
    while (true) {
        auto chunk = co_await stream.read_chunk();
        if (!chunk) {
            // Pipe EOF is reported on the error channel, not as an empty chunk.
            if (chunk.error() == lighter::Error::k_end_of_file) {
                co_return capture;
            }
            co_await fail(std::move(chunk).error());
        }
        if (chunk->empty()) {
            co_return capture;
        }
        capture.append(std::string_view(chunk->data(), chunk->size()));
        stream.consume(chunk->size());
    }
}

struct CommandCompletion {
    BoundedCapture stdout_capture;
    BoundedCapture stderr_capture;
    Process::ExitStatus exit_status;
};

Task<CommandCompletion, lighter::Error> collect_command(Process::SpawnResult &child) {
    auto joined = co_await WhenAll(drain(child.stdout_pipe), drain(child.stderr_pipe), child.proc.wait());
    if (!joined) co_await fail(std::move(joined).error());
    auto [captured_stdout, captured_stderr, exit_status] = *std::move(joined);
    co_return CommandCompletion{
        .stdout_capture = std::move(captured_stdout),
        .stderr_capture = std::move(captured_stderr),
        .exit_status = exit_status,
    };
}

Task<std::string, Error> tool_run_command(const ToolSet &tools, RunCommandInput input) {
#ifdef _WIN32
    Process::Options options{
        .file = "pwsh",
        .args = {"pwsh", "-NoProfile", "-NonInteractive", "-Command", std::move(input.command)},
        .cwd = tools.working_directory.string(),
        .streams = {Process::Stdio::ignore(), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#else
    Process::Options options{
        .file = "/bin/sh",
        .args = {"sh", "-lc", std::move(input.command)},
        .cwd = tools.working_directory.string(),
        .streams = {Process::Stdio::ignore(), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#endif

    auto spawned = Process::spawn(options);
    if (!spawned) {
        co_await fail(Error::tool("failed to spawn command shell: " + std::string(spawned.error().message())));
    }
    auto child = *std::move(spawned);

    // Drain both pipes while waiting; reading only after exit can deadlock
    // once a pipe buffer fills. Cancellation of wait() kills the child.
    auto completed = co_await lighter::with_timeout(collect_command(child), tools.policy.command_timeout);
    if (completed.has_error()) {
        if (completed.error() == lighter::Error::k_connection_timed_out) {
            co_await fail(Error::tool("command exceeded " + std::to_string(tools.policy.command_timeout.count()) + " ms timeout"));
        }
        co_await fail(Error::tool("i/o failure while running command: " + std::string(completed.error().message())));
    }
    if (completed.has_value()) {
        auto [captured_stdout, captured_stderr, exit_status] = *std::move(completed);

        std::string result = "exit_code: " + std::to_string(exit_status.status);
        if (exit_status.term_signal != 0) {
            result += "\nterm_signal: " + std::to_string(exit_status.term_signal);
        }
        result += "\n\nstdout:\n" + std::move(captured_stdout).text();
        result += "\n\nstderr:\n" + std::move(captured_stderr).text();
        co_return result;
    }
    co_await lighter::cancel();
}

} // namespace

ToolCallPresentation describe_tool_call(const provider::ToolCall &call) {
    if (call.name == "read_file") {
        const auto input = parse_input<ReadFileInput>(call.input);
        if (input) return {.description = "Read " + bounded_text(input->path, k_max_call_summary_bytes)};
    }
    if (call.name == "run_command") {
        const auto input = parse_input<RunCommandInput>(call.input);
        if (input) {
            return {.command = bounded_text(normalize_newlines(input->command), k_max_call_summary_bytes)};
        }
    }
    return {.description = "Run " + bounded_text(call.name, k_max_call_summary_bytes)};
}

std::string summarize_tool_result(const provider::ToolCall &call, const provider::ToolResult &result) {
    if (call.name == "read_file") {
        if (result.is_error) return generic_result_summary(result);
        usize lines = 0;
        if (!result.content.empty()) {
            lines = static_cast<usize>(std::ranges::count(result.content, '\n'));
            if (!result.content.ends_with('\n')) ++lines;
        }
        auto summary = line_count(lines) + " · " + byte_count(result.content.size());
        if (result.content.contains("... [truncated after ")) summary += " · truncated";
        return summary;
    }
    if (call.name == "run_command") return command_result_summary(result);
    return generic_result_summary(result);
}

Result<ToolPolicy> load_tool_policy() {
    ToolPolicy policy;
    auto timeout = parse_bounded_environment<i64>("LIMINAL_COMMAND_TIMEOUT_MS", policy.command_timeout.count(), k_min_command_timeout_ms,
                                                  k_max_command_timeout_ms);
    if (!timeout) return outcome_error(std::move(timeout).error());
    policy.command_timeout = std::chrono::milliseconds(*timeout);

    auto parallel = parse_bounded_environment<usize>("LIMINAL_MAX_PARALLEL_TOOLS", policy.max_parallel_calls, 1, k_max_parallel_call_limit);
    if (!parallel) return outcome_error(std::move(parallel).error());
    policy.max_parallel_calls = *parallel;

    auto per_turn = parse_bounded_environment<usize>("LIMINAL_MAX_TOOLS_PER_TURN", policy.max_calls_per_turn, 1, k_max_turn_call_limit);
    if (!per_turn) return outcome_error(std::move(per_turn).error());
    policy.max_calls_per_turn = *per_turn;
    return policy;
}

ToolSet::ToolSet(std::filesystem::path working_directory, ToolPolicy policy)
    : working_directory(std::move(working_directory)), policy(policy) {}

std::vector<provider::ToolDefinition> ToolSet::definitions() const {
    return {
        {
            .name = "read_file",
            .description = "Read a local text file. Use this when you need the exact contents of a "
                           "file before answering or acting.",
            .input_schema = {.properties = {{"path",
                                             {.type = "string",
                                              .description = "Absolute path, or a path relative to the "
                                                             "working directory."}}},
                             .required = {"path"}},
        },
        {
            .name = "run_command",
#ifdef _WIN32
            .description = "Run a PowerShell (pwsh) command in the working directory. Use this to "
                           "inspect the environment, build, test, or perform actions.",
            .input_schema = {.properties = {{"command",
                                             {.type = "string",
                                              .description = "PowerShell command text, passed as a "
                                                             "single argument to pwsh -Command."}}},
                             .required = {"command"}},
#else
            .description = "Run a POSIX sh command in the working directory. Use this to inspect "
                           "the environment, build, test, or perform actions.",
            .input_schema = {.properties = {{"command",
                                             {.type = "string",
                                              .description = "POSIX shell command text, passed as a "
                                                             "single argument to /bin/sh -lc."}}},
                             .required = {"command"}},
#endif
        },
    };
}

Task<provider::ToolResult, Error> ToolSet::execute(const provider::ToolCall &call) const {
    provider::ToolResult result{.call_id = call.id};

    if (call.name == "read_file") {
        auto input = co_await or_fail(parse_input<ReadFileInput>(call.input));
        result.content = tool_read_file(*this, input);
        result.is_error = result.content.starts_with("Error:");
        co_return result;
    }
    if (call.name == "run_command") {
        auto input = co_await or_fail(parse_input<RunCommandInput>(call.input));
        result.content = co_await tool_run_command(*this, std::move(input)).or_fail();
        result.is_error = !result.content.starts_with("exit_code: 0\n");
        co_return result;
    }
    co_await fail(Error::tool("unknown tool: " + call.name));
}

} // namespace liminal
