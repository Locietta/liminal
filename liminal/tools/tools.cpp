#include "tools.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/io/fs.h>
#include <lighter/async/io/process.h>
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

struct ReadFileInput {
    std::string path;
};

struct RunCommandInput {
    std::string command;
};

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

bool is_path_absolute(std::string_view path) {
    if (path.starts_with('/') || path.starts_with('\\')) {
        return true;
    }
    return path.size() >= 2 && path[1] == ':';
}

std::string tool_read_file(const ToolSet &tools, const ReadFileInput &input) {
    std::string path = input.path;
    if (!is_path_absolute(path)) {
        path = tools.working_directory + "/" + path;
    }

    auto content = lighter::fs::sync::read_to_string(path);
    if (!content) {
        return "Error: cannot read '" + path + "': " + std::string(content.error().message());
    }
    if (content->find('\0') != std::string::npos) {
        return "Error: '" + path + "' looks like a binary file";
    }
    if (content->size() > k_file_limit) {
        auto truncated = content->substr(0, k_file_limit);
        truncated += "\n... [truncated: " + std::to_string(content->size() - k_file_limit) + " more bytes]";
        return truncated;
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

Task<std::string, Error> tool_run_command(const ToolSet &tools, RunCommandInput input) {
#ifdef _WIN32
    Process::Options options{
        .file = "pwsh",
        .args = {"pwsh", "-NoProfile", "-NonInteractive", "-Command", std::move(input.command)},
        .cwd = tools.working_directory,
        .streams = {Process::Stdio::ignore(), Process::Stdio::pipe(false, true), Process::Stdio::pipe(false, true)},
    };
#else
    Process::Options options{
        .file = "/bin/sh",
        .args = {"sh", "-lc", std::move(input.command)},
        .cwd = tools.working_directory,
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
    auto joined = co_await WhenAll(drain(child.stdout_pipe), drain(child.stderr_pipe), child.proc.wait());
    if (!joined) {
        co_await fail(Error::tool("i/o failure while running command: " + std::string(joined.error().message())));
    }
    auto [captured_stdout, captured_stderr, exit_status] = *std::move(joined);

    std::string result = "exit_code: " + std::to_string(exit_status.status);
    if (exit_status.term_signal != 0) {
        result += "\nterm_signal: " + std::to_string(exit_status.term_signal);
    }
    result += "\n\nstdout:\n" + std::move(captured_stdout).text();
    result += "\n\nstderr:\n" + std::move(captured_stderr).text();
    co_return result;
}

} // namespace

ToolSet::ToolSet(std::string working_directory) : working_directory(std::move(working_directory)) {}

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
        co_return result;
    }
    co_await fail(Error::tool("unknown tool: " + call.name));
}

} // namespace liminal
