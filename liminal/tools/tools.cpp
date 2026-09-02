#include "tools.h"

#include "apply_patch.h"
#include "exec.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/codec/json/json.h>
#include <lighter/utils/panic.h>

#include <liminal/text.h>

namespace liminal {

namespace json = lighter::codec::json;
using lighter::fail;
using lighter::or_fail;
using lighter::outcome_error;
using lighter::Task;

void finalize_tool_outcome(ToolOutcome &outcome, ToolOutputGrant grant) {
    grant.payload_bytes = std::min(grant.payload_bytes, k_max_tool_payload_bytes);
    outcome.receipt = lighter::encoding::utf8::sanitize(outcome.receipt);
    lighter::check(outcome.receipt.size() <= grant.receipt_bytes, "tool outcome exceeded its reserved receipt capacity");
    outcome.payload = lighter::encoding::utf8::sanitize(outcome.payload);
    if (outcome.payload.size() <= grant.payload_bytes) return;
    outcome.payload = bounded_utf8(outcome.payload, grant.payload_bytes);
    outcome.payload_truncated = true;
}

namespace {

constexpr usize k_read_notice_reserve_bytes = 256;
constexpr std::string_view k_insufficient_output_capacity = "Error: insufficient output capacity; compact first";
constexpr usize k_max_call_summary_bytes = 2 * 1024;
constexpr usize k_max_preview_line_bytes = 240;
constexpr usize k_max_preview_lines = 4;

struct ReadFileInput {
    std::string path;
    std::optional<usize> offset;
    std::optional<usize> limit;
};

struct ReadFileBytesInput {
    std::string path;
    std::optional<u64> byte_offset;
    std::optional<usize> byte_count;
};

/// Result of a bounded read. `continuation` is the control-plane note that
/// tells the model how to resume when the payload stopped short of the
/// requested range; it stays separate from the file content so payload text
/// can never be mistaken for it.
struct ReadResult {
    std::string payload;
    std::string continuation;
};

std::string bounded_text(std::string_view text, usize limit) {
    if (text.size() <= limit) return std::string(text);
    auto end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + "…";
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

std::string generic_result_summary(const ToolOutcome &outcome) {
    const auto content = outcome.receipt + (outcome.receipt.empty() || outcome.payload.empty() ? "" : "\n") + outcome.payload;
    const auto lines = content_lines(content);
    if (lines.empty()) return tool_outcome_is_error(outcome.kind) ? "No error detail" : "No output";
    std::string summary = tool_outcome_is_error(outcome.kind) ? "Error" : line_count(lines.size());
    for (const auto &line : preview_lines(lines)) summary += "\n" + line;
    return summary;
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

Result<void> validate_read_file_input(const ReadFileInput &input) {
    if (input.path.empty()) return outcome_error(Error::tool("path must not be empty"));
    if (input.offset.value_or(1) == 0) return outcome_error(Error::tool("offset must be at least 1"));
    if (input.limit && *input.limit == 0) return outcome_error(Error::tool("limit must be at least 1"));
    return {};
}

Result<void> validate_read_file_bytes_input(const ReadFileBytesInput &input) {
    if (input.path.empty()) return outcome_error(Error::tool("path must not be empty"));
    if (input.byte_count && *input.byte_count == 0) return outcome_error(Error::tool("byte_count must be at least 1"));
    return {};
}

Result<std::filesystem::path> resolve_read_path(const ToolSet &tools, std::string_view path) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(tools.working_directory, error);
    if (error) {
        return outcome_error(Error::tool("cannot resolve workspace: " + error.message()));
    }

    auto requested = std::filesystem::path(path);
    if (requested.is_relative()) requested = root / requested;
    auto resolved = std::filesystem::weakly_canonical(requested, error);
    if (error) {
        return outcome_error(Error::tool("cannot resolve '" + requested.string() + "': " + error.message()));
    }
    return resolved;
}

Result<ReadResult> read_byte_range(const std::filesystem::path &path, u64 offset, usize count_limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return outcome_error(Error::tool("cannot open '" + path.string() + "'"));

    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0) return outcome_error(Error::tool("cannot size '" + path.string() + "'"));
    const auto size = static_cast<u64>(end);
    if (offset > size) {
        return outcome_error(Error::tool("byte_offset " + std::to_string(offset) + " exceeds file size " + std::to_string(size)));
    }
    if (offset > static_cast<u64>(std::numeric_limits<std::streamoff>::max())) {
        return outcome_error(Error::tool("byte_offset is too large for this platform"));
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    const auto available = size - offset;
    const auto count = static_cast<usize>(std::min<u64>(available, count_limit));
    std::string content(count, '\0');
    stream.read(content.data(), static_cast<std::streamsize>(count));
    content.resize(static_cast<usize>(stream.gcount()));
    if (!stream && !stream.eof()) return outcome_error(Error::tool("cannot read '" + path.string() + "'"));
    if (content.find('\0') != std::string::npos) return outcome_error(Error::tool("'" + path.string() + "' looks like a binary file"));
    if (!content.empty() && (static_cast<unsigned char>(content.front()) & 0xc0) == 0x80) {
        return outcome_error(Error::tool("byte_offset " + std::to_string(offset) + " falls inside a UTF-8 code point"));
    }
    ReadResult result;
    if (available > content.size()) {
        const auto complete = lighter::encoding::utf8::complete_prefix_len(content);
        if (complete == 0 && !content.empty()) {
            return outcome_error(Error::tool("byte_count is too small to include the UTF-8 code point at byte_offset " +
                                             std::to_string(offset) + "; increase byte_count"));
        }
        content.resize(complete);
        result.continuation =
            "truncated after " + std::to_string(complete) + " bytes; next byte_offset " + std::to_string(offset + complete);
    }
    result.payload = std::move(content);
    return result;
}

Result<ReadResult> read_line_range(const std::filesystem::path &path, usize first, std::optional<usize> line_limit, usize output_limit) {
    if (first == 0) return outcome_error(Error::tool("offset must be at least 1"));
    if (line_limit && *line_limit == 0) return outcome_error(Error::tool("limit must be at least 1"));

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return outcome_error(Error::tool("cannot open '" + path.string() + "'"));

    ReadResult result;
    auto &content = result.payload;
    usize line_number = 0;
    usize selected_lines = 0;
    bool truncated = false;
    bool limited = false;
    usize continuation_line = 0;
    const auto oversized_line_marker = [&](std::streampos line_offset) {
        auto marker = "truncated before oversized line " + std::to_string(line_number) + "; use read_file_bytes";
        if (line_offset >= 0) marker += " with byte_offset " + std::to_string(static_cast<u64>(line_offset));
        return marker;
    };
    while (true) {
        const auto line_offset = stream.tellg();
        const bool selected = line_number + 1 >= first;
        std::string line;
        bool has_bytes = false;
        bool reached_eof = false;
        bool oversized = false;
        while (true) {
            const auto next = stream.get();
            if (next == std::char_traits<char>::eof()) {
                reached_eof = true;
                break;
            }
            has_bytes = true;
            const auto byte = static_cast<char>(next);
            if (byte == '\n') break;
            if (!selected) continue;
            if (byte == '\0') return outcome_error(Error::tool("'" + path.string() + "' looks like a binary file"));
            if (line.size() >= output_limit) {
                oversized = true;
                break;
            }
            line += byte;
        }
        if (!has_bytes && reached_eof) break;
        ++line_number;
        if (line_number < first) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (oversized || line.size() + 1 > output_limit) {
            auto marker = oversized_line_marker(line_offset);
            if (content.empty()) return outcome_error(Error::tool(std::move(marker)));
            result.continuation = std::move(marker);
            return result;
        }
        if (line.size() + 1 > output_limit - std::min(content.size(), output_limit)) {
            truncated = true;
            break;
        }
        content += line;
        content += '\n';
        ++selected_lines;
        if (line_limit && selected_lines >= *line_limit) {
            if (stream.peek() != std::char_traits<char>::eof()) {
                limited = true;
                continuation_line = line_number + 1;
            }
            break;
        }
        if (reached_eof) break;
    }
    if (stream.bad()) {
        return outcome_error(Error::tool("cannot read '" + path.string() + "'"));
    }
    if (truncated) {
        result.continuation =
            "truncated before line " + std::to_string(line_number) + "; continue with offset " + std::to_string(line_number);
    } else if (limited) {
        result.continuation = "line limit reached before line " + std::to_string(continuation_line) + "; continue with offset " +
                              std::to_string(continuation_line);
    }
    return result;
}

Result<std::filesystem::path> resolve_regular_file(const ToolSet &tools, std::string_view requested) {
    auto resolved = resolve_read_path(tools, requested);
    if (!resolved) return resolved;
    const auto path = resolved->string();

    std::error_code status_error;
    const auto status = std::filesystem::status(*resolved, status_error);
    if (status_error) {
        return outcome_error(Error::tool("cannot inspect '" + path + "': " + status_error.message()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return outcome_error(Error::tool("'" + path + "' is not a regular file"));
    }
    return resolved;
}

Result<ReadResult> tool_read_file(const ToolSet &tools, const ReadFileInput &input, ToolOutputGrant grant) {
    if (auto valid = validate_read_file_input(input); !valid) return outcome_error(std::move(valid).error());
    auto resolved = resolve_regular_file(tools, input.path);
    if (!resolved) return outcome_error(std::move(resolved).error());
    return read_line_range(*resolved, input.offset.value_or(1), input.limit, grant.payload_bytes);
}

Result<ReadResult> tool_read_file_bytes(const ToolSet &tools, const ReadFileBytesInput &input, ToolOutputGrant grant) {
    if (auto valid = validate_read_file_bytes_input(input); !valid) return outcome_error(std::move(valid).error());
    auto resolved = resolve_regular_file(tools, input.path);
    if (!resolved) return outcome_error(std::move(resolved).error());
    const auto requested_count = input.byte_count.value_or(grant.payload_bytes);
    return read_byte_range(*resolved, input.byte_offset.value_or(0), std::min(requested_count, grant.payload_bytes));
}

/// Projects a read onto the tool outcome: file content is the payload, the
/// resume note is the receipt, and a failure carries its message as payload.
ToolOutcome read_outcome(std::string call_id, Result<ReadResult> read) {
    ToolOutcome outcome{.call_id = std::move(call_id)};
    if (!read) {
        outcome.kind = ToolOutcomeKind::FAILED;
        outcome.payload = "Error: " + read.error().message();
        return outcome;
    }
    outcome.payload = std::move(read->payload);
    if (!read->continuation.empty()) outcome.receipt = "continuation: " + read->continuation;
    return outcome;
}

} // namespace

namespace {

ToolCallPresentation describe_read_file(const provider::ToolCall &call) {
    const auto input = parse_input<ReadFileInput>(call.input);
    if (!input) return {.description = "Read file"};
    auto description = "Read " + bounded_text(input->path, k_max_call_summary_bytes);
    if (input->offset || input->limit) {
        description += " lines from " + std::to_string(input->offset.value_or(1));
        if (input->limit) description += " limit " + std::to_string(*input->limit);
    }
    return {.description = std::move(description)};
}

ToolCallPresentation describe_read_file_bytes(const provider::ToolCall &call) {
    const auto input = parse_input<ReadFileBytesInput>(call.input);
    if (!input) return {.description = "Read file bytes"};
    auto description =
        "Read " + bounded_text(input->path, k_max_call_summary_bytes) + " bytes from " + std::to_string(input->byte_offset.value_or(0));
    if (input->byte_count) description += " count " + std::to_string(*input->byte_count);
    return {.description = std::move(description)};
}

std::string summarize_read_file(const provider::ToolCall &, const ToolOutcome &result) {
    if (tool_outcome_is_error(result.kind)) return generic_result_summary(result);
    usize lines = 0;
    if (!result.payload.empty()) {
        lines = static_cast<usize>(std::ranges::count(result.payload, '\n'));
        if (!result.payload.ends_with('\n')) ++lines;
    }
    auto summary = line_count(lines) + " · " + byte_count(result.payload.size());
    if (result.payload_truncated || result.receipt.contains("continuation: truncated ")) summary += " · truncated";
    return summary;
}

ToolRegistration read_file_registration() {
    return {
        .definition =
            {
                .name = "read_file",
                .description = "Read a local text file by line. Offset is one-based and limit is a line count. Omit both to read from "
                               "the beginning. Output is bounded by the current context budget; use read_file_bytes for large generated "
                               "files.",
                .input_schema =
                    {
                        .properties =
                            {
                                {"path", {.type = "string", .description = "Absolute path, or a path relative to the working directory."}},
                                {"offset", {.type = "integer", .description = "One-based first line. Defaults to 1."}},
                                {"limit", {.type = "integer", .description = "Maximum number of lines to return. Omit to read onward."}},
                            },
                        .required = {"path"},
                    },
            },
        .execution_mode = ToolExecutionMode::PARALLEL,
        .receipt_bytes = k_read_notice_reserve_bytes,
        .minimum_payload_bytes = 4,
        .validate = [](const provider::ToolCall &call) -> Result<void> {
            auto input = parse_input<ReadFileInput>(call.input);
            if (!input) return outcome_error(std::move(input).error());
            return validate_read_file_input(*input);
        },
        .execute = [](const ToolSet &tools, const provider::ToolCall &call, ToolOutputGrant grant) -> Task<ToolOutcome, Error> {
            auto input = co_await or_fail(parse_input<ReadFileInput>(call.input));
            co_return read_outcome(call.id, tool_read_file(tools, input, grant));
        },
        .describe = describe_read_file,
        .summarize = summarize_read_file,
    };
}

ToolRegistration read_file_bytes_registration() {
    return {
        .definition =
            {
                .name = "read_file_bytes",
                .description = "Read a bounded byte range from a local text file. Use this for large generated files or to continue "
                               "from a byte truncation marker; use read_file for ordinary source inspection. Actual output is bounded "
                               "by the current context budget.",
                .input_schema =
                    {
                        .properties =
                            {
                                {"path", {.type = "string", .description = "Absolute path, or a path relative to the working directory."}},
                                {"byte_offset", {.type = "integer", .description = "Zero-based byte offset. Defaults to 0."}},
                                {"byte_count",
                                 {.type = "integer", .description = "Requested number of bytes. Defaults to the current output budget."}},
                            },
                        .required = {"path"},
                    },
            },
        .execution_mode = ToolExecutionMode::PARALLEL,
        .receipt_bytes = k_read_notice_reserve_bytes,
        .minimum_payload_bytes = 4,
        .validate = [](const provider::ToolCall &call) -> Result<void> {
            auto input = parse_input<ReadFileBytesInput>(call.input);
            if (!input) return outcome_error(std::move(input).error());
            return validate_read_file_bytes_input(*input);
        },
        .execute = [](const ToolSet &tools, const provider::ToolCall &call, ToolOutputGrant grant) -> Task<ToolOutcome, Error> {
            auto input = co_await or_fail(parse_input<ReadFileBytesInput>(call.input));
            co_return read_outcome(call.id, tool_read_file_bytes(tools, input, grant));
        },
        .describe = describe_read_file_bytes,
        .summarize = summarize_read_file,
    };
}

const ToolRegistration *find_registration(const std::vector<std::unique_ptr<ToolRegistration>> &registrations, std::string_view name) {
    const auto found = std::ranges::find(registrations, name, [](const auto &tool) { return std::string_view(tool->definition.name); });
    return found == registrations.end() ? nullptr : found->get();
}

ToolCallPresentation fallback_description(const provider::ToolCall &call) {
    if (call.name == "read_file") {
        return {.description = "Read file"};
    }
    if (call.name == "read_file_bytes") {
        return {.description = "Read file bytes"};
    }
    return {.description = "Run " + bounded_text(call.name, k_max_call_summary_bytes)};
}

} // namespace

ToolSet::ToolSet(std::filesystem::path working_directory)
    : working_directory(std::move(working_directory)), shell_tasks(make_shell_task_manager(this->working_directory)) {
    lighter::check(static_cast<bool>(register_tool(read_file_registration())), "failed to register read_file");
    lighter::check(static_cast<bool>(register_tool(read_file_bytes_registration())), "failed to register read_file_bytes");
    lighter::check(static_cast<bool>(register_tool(make_apply_patch_tool())), "failed to register apply_patch");
    for (auto &tool : make_exec_tools(*shell_tasks)) {
        lighter::check(static_cast<bool>(register_tool(std::move(tool))), "failed to register exec tool");
    }
}

ToolSet::~ToolSet() = default;

Result<void> ToolSet::register_tool(ToolRegistration tool) {
    if (tool.definition.name.empty()) return outcome_error(Error::config("tool name cannot be empty"));
    if (!tool.prepare && !tool.execute) return outcome_error(Error::config("tool '" + tool.definition.name + "' has no executor"));
    if (tool.receipt_bytes > k_max_tool_receipt_bytes) {
        return outcome_error(Error::config("tool '" + tool.definition.name + "' reserves too much receipt capacity"));
    }
    if (tool.minimum_payload_bytes > k_max_tool_payload_bytes) {
        return outcome_error(Error::config("tool '" + tool.definition.name + "' requires too much payload capacity"));
    }
    if (find_registration(registrations, tool.definition.name)) {
        return outcome_error(Error::config("duplicate tool name: " + tool.definition.name));
    }
    registrations.push_back(std::make_unique<ToolRegistration>(std::move(tool)));
    return {};
}

std::vector<provider::ToolDefinition> ToolSet::definitions() const {
    std::vector<provider::ToolDefinition> result;
    result.reserve(registrations.size() + 2);
    for (const auto &tool : registrations) result.push_back(tool->definition);
    result.push_back({.kind = provider::ToolKind::WEB_SEARCH, .name = "web_search", .description = "Search the public web."});
    result.push_back({.kind = provider::ToolKind::WEB_FETCH, .name = "web_fetch", .description = "Fetch a public web page."});
    return result;
}

Result<PreparedToolCall> ToolSet::prepare(provider::ToolCall call) const {
    const auto *tool = find_registration(registrations, call.name);
    if (!tool) return outcome_error(Error::tool("unknown tool: " + call.name));
    if (tool->prepare) {
        auto execution = tool->prepare(*this, call);
        if (!execution) return outcome_error(std::move(execution).error());
        if (!execution->execute) return outcome_error(Error::tool("tool '" + call.name + "' prepared no executor"));
        if (execution->receipt_bytes > k_max_tool_receipt_bytes) {
            return outcome_error(Error::tool("prepared receipt exceeds the local tool limit"));
        }
        if (execution->minimum_payload_bytes > k_max_tool_payload_bytes) {
            return outcome_error(Error::tool("prepared minimum payload exceeds the local tool limit"));
        }
        return PreparedToolCall{.call = std::move(call),
                                .mode = tool->execution_mode,
                                .receipt_bytes = execution->receipt_bytes,
                                .minimum_payload_bytes = execution->minimum_payload_bytes,
                                .execute = std::move(execution->execute)};
    }
    if (tool->validate) {
        auto valid = tool->validate(call);
        if (!valid) return outcome_error(std::move(valid).error());
    }
    auto invocation = call;
    return PreparedToolCall{
        .call = std::move(call),
        .mode = tool->execution_mode,
        .receipt_bytes = tool->receipt_bytes,
        .minimum_payload_bytes = tool->minimum_payload_bytes,
        .execute = [this, tool,
                    invocation = std::move(invocation)](ToolOutputGrant grant) mutable { return tool->execute(*this, invocation, grant); },
    };
}

Task<ToolOutcome, Error> ToolSet::execute(provider::ToolCall call, ToolOutputGrant grant) const {
    auto prepared = co_await or_fail(prepare(std::move(call)));
    lighter::check(grant.receipt_bytes >= prepared.receipt_bytes, "tool grant does not cover its prepared receipt");
    lighter::check(grant.payload_bytes >= prepared.minimum_payload_bytes, "tool grant does not cover its minimum payload");
    auto outcome = co_await prepared.execute(grant).or_fail();
    finalize_tool_outcome(outcome, grant);
    co_return outcome;
}

ToolExecutionMode ToolSet::execution_mode(std::string_view name) const {
    const auto *tool = find_registration(registrations, name);
    return tool ? tool->execution_mode : ToolExecutionMode::EXCLUSIVE;
}

ToolCallPresentation ToolSet::describe(const provider::ToolCall &call) const {
    const auto *tool = find_registration(registrations, call.name);
    return tool && tool->describe ? tool->describe(call) : fallback_description(call);
}

std::string ToolSet::summarize(const provider::ToolCall &call, const ToolOutcome &outcome) const {
    const auto *tool = find_registration(registrations, call.name);
    return tool && tool->summarize ? tool->summarize(call, outcome) : generic_result_summary(outcome);
}

} // namespace liminal
