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

namespace liminal {

namespace json = lighter::codec::json;
using lighter::fail;
using lighter::or_fail;
using lighter::outcome_error;
using lighter::Task;

namespace {

constexpr usize k_file_limit = 128 * 1024;
constexpr usize k_max_call_summary_bytes = 2 * 1024;
constexpr usize k_max_preview_line_bytes = 240;
constexpr usize k_max_preview_lines = 4;

struct ReadFileInput {
    std::string path;
    std::optional<u64> offset;
    std::optional<usize> limit;
    std::optional<usize> line_start;
    std::optional<usize> line_end;
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

std::string generic_result_summary(const provider::ToolResult &result) {
    const auto lines = content_lines(result.content);
    if (lines.empty()) return result.is_error ? "No error detail" : "No output";
    std::string summary = result.is_error ? "Error" : line_count(lines.size());
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

Result<std::string> read_byte_range(const std::filesystem::path &path, u64 offset, usize limit) {
    if (limit == 0) return outcome_error(Error::tool("limit must be at least 1"));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return outcome_error(Error::tool("cannot open '" + path.string() + "'"));

    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0) return outcome_error(Error::tool("cannot size '" + path.string() + "'"));
    const auto size = static_cast<u64>(end);
    if (offset > size) {
        return outcome_error(Error::tool("offset " + std::to_string(offset) + " exceeds file size " + std::to_string(size)));
    }
    if (offset > static_cast<u64>(std::numeric_limits<std::streamoff>::max())) {
        return outcome_error(Error::tool("offset is too large for this platform"));
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    const auto available = size - offset;
    const auto count = static_cast<usize>(std::min<u64>(available, limit));
    std::string content(count, '\0');
    stream.read(content.data(), static_cast<std::streamsize>(count));
    content.resize(static_cast<usize>(stream.gcount()));
    if (!stream && !stream.eof()) return outcome_error(Error::tool("cannot read '" + path.string() + "'"));
    if (content.find('\0') != std::string::npos) return outcome_error(Error::tool("'" + path.string() + "' looks like a binary file"));
    if (available > count) {
        content += "\n... [truncated after " + std::to_string(count) + " bytes; next offset " + std::to_string(offset + count) + "]";
    }
    return content;
}

Result<std::string> read_line_range(const std::filesystem::path &path, usize first, std::optional<usize> last) {
    if (first == 0) return outcome_error(Error::tool("line_start must be at least 1"));
    if (last && (*last == 0 || *last < first)) {
        return outcome_error(Error::tool("line_end must be at least line_start"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return outcome_error(Error::tool("cannot open '" + path.string() + "'"));

    std::string content;
    std::string line;
    usize line_number = 0;
    bool truncated = false;
    while (std::getline(stream, line)) {
        ++line_number;
        if (line_number < first) continue;
        if (last && line_number > *last) break;
        if (line.find('\0') != std::string::npos) return outcome_error(Error::tool("'" + path.string() + "' looks like a binary file"));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (content.size() + line.size() + 1 > k_file_limit) {
            truncated = true;
            break;
        }
        content += line;
        content += '\n';
    }
    if (!stream.eof() && stream.fail() && !(last && line_number > *last)) {
        return outcome_error(Error::tool("cannot read '" + path.string() + "'"));
    }
    if (truncated) {
        content +=
            "... [truncated before line " + std::to_string(line_number) + "; continue with line_start " + std::to_string(line_number) + "]";
    }
    return content;
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

    const bool lines = input.line_start.has_value() || input.line_end.has_value();
    const bool bytes = input.offset.has_value() || input.limit.has_value();
    if (lines && bytes) return "Error: line ranges cannot be combined with byte offsets";

    const auto byte_limit = std::min(input.limit.value_or(k_file_limit), k_file_limit);
    Result<std::string> content = lines ? read_line_range(*resolved, input.line_start.value_or(1), input.line_end) :
                                          read_byte_range(*resolved, input.offset.value_or(0), byte_limit);
    if (!content) return "Error: " + content.error().message();
    return *std::move(content);
}

} // namespace

namespace {

ToolCallPresentation describe_read_file(const provider::ToolCall &call) {
    const auto input = parse_input<ReadFileInput>(call.input);
    if (!input) return {.description = "Read file"};
    auto description = "Read " + bounded_text(input->path, k_max_call_summary_bytes);
    if (input->line_start || input->line_end) {
        description += " lines " + std::to_string(input->line_start.value_or(1));
        description += input->line_end ? "-" + std::to_string(*input->line_end) : "+";
    } else if (input->offset || input->limit) {
        description += " bytes from " + std::to_string(input->offset.value_or(0));
        if (input->limit) description += " limit " + std::to_string(*input->limit);
    }
    return {.description = std::move(description)};
}

std::string summarize_read_file(const provider::ToolCall &, const provider::ToolResult &result) {
    if (result.is_error) return generic_result_summary(result);
    usize lines = 0;
    if (!result.content.empty()) {
        lines = static_cast<usize>(std::ranges::count(result.content, '\n'));
        if (!result.content.ends_with('\n')) ++lines;
    }
    auto summary = line_count(lines) + " · " + byte_count(result.content.size());
    if (result.content.contains("... [truncated ")) summary += " · truncated";
    return summary;
}

ToolRegistration read_file_registration() {
    return {
        .definition =
            {
                .name = "read_file",
                .description = "Read a local text file or a bounded byte/line range. Use byte offsets for large generated files and "
                               "one-based inclusive line ranges for source inspection. Do not combine byte and line ranges.",
                .input_schema =
                    {
                        .properties =
                            {
                                {"path", {.type = "string", .description = "Absolute path, or a path relative to the working directory."}},
                                {"offset", {.type = "integer", .description = "Zero-based byte offset. Defaults to 0."}},
                                {"limit",
                                 {.type = "integer", .description = "Maximum bytes to return, capped at 131072. Defaults to 131072."}},
                                {"line_start", {.type = "integer", .description = "One-based first line. Defaults to 1."}},
                                {"line_end", {.type = "integer", .description = "One-based inclusive last line. Omit to read onward."}},
                            },
                        .required = {"path"},
                    },
            },
        .execution_mode = ToolExecutionMode::PARALLEL,
        .execute = [](const ToolSet &tools, const provider::ToolCall &call) -> Task<provider::ToolResult, Error> {
            auto input = co_await or_fail(parse_input<ReadFileInput>(call.input));
            provider::ToolResult result{.call_id = call.id};
            result.content = tool_read_file(tools, input);
            result.is_error = result.content.starts_with("Error:");
            co_return result;
        },
        .describe = describe_read_file,
        .summarize = summarize_read_file,
    };
}

const ToolRegistration *find_registration(const std::vector<ToolRegistration> &registrations, std::string_view name) {
    const auto found = std::ranges::find(registrations, name, [](const auto &tool) { return std::string_view(tool.definition.name); });
    return found == registrations.end() ? nullptr : &*found;
}

ToolCallPresentation fallback_description(const provider::ToolCall &call) {
    if (call.name == "read_file") {
        return {.description = "Read file"};
    }
    return {.description = "Run " + bounded_text(call.name, k_max_call_summary_bytes)};
}

} // namespace

ToolSet::ToolSet(std::filesystem::path working_directory)
    : working_directory(std::move(working_directory)), shell_tasks(make_shell_task_manager(this->working_directory)) {
    lighter::check(static_cast<bool>(register_tool(read_file_registration())), "failed to register read_file");
    lighter::check(static_cast<bool>(register_tool(make_apply_patch_tool())), "failed to register apply_patch");
    for (auto &tool : make_exec_tools(*shell_tasks)) {
        lighter::check(static_cast<bool>(register_tool(std::move(tool))), "failed to register exec tool");
    }
}

ToolSet::~ToolSet() = default;

Result<void> ToolSet::register_tool(ToolRegistration tool) {
    if (tool.definition.name.empty()) return outcome_error(Error::config("tool name cannot be empty"));
    if (!tool.execute) return outcome_error(Error::config("tool '" + tool.definition.name + "' has no executor"));
    if (find_registration(registrations, tool.definition.name)) {
        return outcome_error(Error::config("duplicate tool name: " + tool.definition.name));
    }
    registrations.push_back(std::move(tool));
    return {};
}

std::vector<provider::ToolDefinition> ToolSet::definitions() const {
    std::vector<provider::ToolDefinition> result;
    result.reserve(registrations.size() + 2);
    for (const auto &tool : registrations) result.push_back(tool.definition);
    result.push_back({.kind = provider::ToolKind::WEB_SEARCH, .name = "web_search", .description = "Search the public web."});
    result.push_back({.kind = provider::ToolKind::WEB_FETCH, .name = "web_fetch", .description = "Fetch a public web page."});
    return result;
}

Task<provider::ToolResult, Error> ToolSet::execute(const provider::ToolCall &call) const {
    if (const auto *tool = find_registration(registrations, call.name)) {
        co_return co_await tool->execute(*this, call).or_fail();
    }
    co_await fail(Error::tool("unknown tool: " + call.name));
}

ToolExecutionMode ToolSet::execution_mode(std::string_view name) const {
    const auto *tool = find_registration(registrations, name);
    return tool ? tool->execution_mode : ToolExecutionMode::EXCLUSIVE;
}

ToolCallPresentation ToolSet::describe(const provider::ToolCall &call) const {
    const auto *tool = find_registration(registrations, call.name);
    return tool && tool->describe ? tool->describe(call) : fallback_description(call);
}

std::string ToolSet::summarize(const provider::ToolCall &call, const provider::ToolResult &result) const {
    const auto *tool = find_registration(registrations, call.name);
    return tool && tool->summarize ? tool->summarize(call, result) : generic_result_summary(result);
}

} // namespace liminal
