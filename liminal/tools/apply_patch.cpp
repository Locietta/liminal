#include "apply_patch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <lighter/async/vocab/outcome.h>
#include <lighter/codec/json/json.h>

namespace liminal {

namespace json = lighter::codec::json;
using lighter::outcome_error;
using lighter::Task;

namespace {

constexpr usize k_max_patch_bytes = 4 * 1024 * 1024;
constexpr usize k_max_edited_file_bytes = 16 * 1024 * 1024;

struct ApplyPatchInput {
    std::string patch;
};

struct UpdateChunk {
    std::optional<std::string> context;
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    std::vector<std::pair<usize, usize>> context_indices;
    bool end_of_file = false;
};

enum struct PatchKind { ADD_FILE, DELETE_FILE, UPDATE_FILE };

struct PatchOperation {
    PatchKind kind;
    std::filesystem::path path;
    std::optional<std::filesystem::path> move_path;
    std::string added_content;
    std::vector<UpdateChunk> chunks;
};

struct TextFile {
    std::vector<std::string> lines;
    std::string newline = "\n";
    bool trailing_newline = false;
};

std::string normalize_patch(std::string_view text) {
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

std::vector<std::string_view> split_patch_lines(std::string_view patch) {
    std::vector<std::string_view> lines;
    usize start = 0;
    while (start < patch.size()) {
        const auto end = patch.find('\n', start);
        lines.push_back(patch.substr(start, end == std::string_view::npos ? patch.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

bool operation_header(std::string_view line) {
    return line.starts_with("*** Add File: ") || line.starts_with("*** Delete File: ") || line.starts_with("*** Update File: ");
}

Result<std::filesystem::path> parse_path(std::string_view text, usize line) {
    if (text.empty()) return outcome_error(Error::tool("invalid patch at line " + std::to_string(line) + ": empty file path"));
    if (text.find('\0') != std::string_view::npos) {
        return outcome_error(Error::tool("invalid patch at line " + std::to_string(line) + ": file path contains NUL"));
    }
    return std::filesystem::path(text);
}

Result<std::vector<PatchOperation>> parse_patch(std::string_view raw) {
    if (raw.size() > k_max_patch_bytes) return outcome_error(Error::tool("patch exceeds 4 MiB"));
    auto normalized = normalize_patch(raw);
    const auto lines = split_patch_lines(normalized);
    if (lines.empty() || lines.front() != "*** Begin Patch") {
        return outcome_error(Error::tool("the first line of the patch must be '*** Begin Patch'"));
    }
    if (lines.back() != "*** End Patch") {
        return outcome_error(Error::tool("the last line of the patch must be '*** End Patch'"));
    }

    std::vector<PatchOperation> operations;
    usize index = 1;
    while (index + 1 < lines.size()) {
        const auto header_line = index + 1;
        const auto header = lines[index++];
        PatchOperation operation{};
        if (header.starts_with("*** Add File: ")) {
            operation.kind = PatchKind::ADD_FILE;
            auto path = parse_path(header.substr(std::string_view("*** Add File: ").size()), header_line);
            if (!path) return outcome_error(std::move(path).error());
            operation.path = *std::move(path);
            while (index + 1 < lines.size() && !operation_header(lines[index])) {
                if (!lines[index].starts_with('+')) {
                    return outcome_error(Error::tool("invalid add-file line " + std::to_string(index + 1) + ": expected '+'"));
                }
                operation.added_content += lines[index++].substr(1);
                operation.added_content += '\n';
            }
            if (operation.added_content.empty()) {
                return outcome_error(Error::tool("add-file operation at line " + std::to_string(header_line) + " has no content"));
            }
        } else if (header.starts_with("*** Delete File: ")) {
            operation.kind = PatchKind::DELETE_FILE;
            auto path = parse_path(header.substr(std::string_view("*** Delete File: ").size()), header_line);
            if (!path) return outcome_error(std::move(path).error());
            operation.path = *std::move(path);
        } else if (header.starts_with("*** Update File: ")) {
            operation.kind = PatchKind::UPDATE_FILE;
            auto path = parse_path(header.substr(std::string_view("*** Update File: ").size()), header_line);
            if (!path) return outcome_error(std::move(path).error());
            operation.path = *std::move(path);
            if (index + 1 < lines.size() && lines[index].starts_with("*** Move to: ")) {
                auto move = parse_path(lines[index].substr(std::string_view("*** Move to: ").size()), index + 1);
                if (!move) return outcome_error(std::move(move).error());
                operation.move_path = *std::move(move);
                ++index;
            }

            UpdateChunk *chunk = nullptr;
            while (index + 1 < lines.size() && !operation_header(lines[index])) {
                const auto line = lines[index++];
                if (line == "@@" || line.starts_with("@@ ")) {
                    operation.chunks.push_back({});
                    chunk = &operation.chunks.back();
                    if (line.size() > 3) chunk->context = std::string(line.substr(3));
                    continue;
                }
                if (!chunk) {
                    operation.chunks.push_back({});
                    chunk = &operation.chunks.back();
                }
                if (line == "*** End of File") {
                    chunk->end_of_file = true;
                } else if (line.starts_with('+')) {
                    chunk->new_lines.emplace_back(line.substr(1));
                } else if (line.starts_with('-')) {
                    chunk->old_lines.emplace_back(line.substr(1));
                } else if (line.starts_with(' ')) {
                    chunk->context_indices.emplace_back(chunk->old_lines.size(), chunk->new_lines.size());
                    chunk->old_lines.emplace_back(line.substr(1));
                    chunk->new_lines.emplace_back(line.substr(1));
                } else {
                    return outcome_error(
                        Error::tool("invalid update line " + std::to_string(index) + ": expected '@@', '+', '-', or space"));
                }
            }
            if (operation.chunks.empty() && !operation.move_path) {
                return outcome_error(Error::tool("update-file operation at line " + std::to_string(header_line) + " has no changes"));
            }
        } else {
            return outcome_error(Error::tool("invalid patch operation at line " + std::to_string(header_line)));
        }
        operations.push_back(std::move(operation));
    }
    if (operations.empty()) return outcome_error(Error::tool("patch contains no file operations"));
    return operations;
}

std::filesystem::path resolve_path(const ToolSet &tools, const std::filesystem::path &path) {
    return path.is_absolute() ? path.lexically_normal() : (tools.working_directory / path).lexically_normal();
}

Result<std::string> read_text_file(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error) return outcome_error(Error::tool("cannot inspect '" + path.string() + "': " + error.message()));
    if (!std::filesystem::is_regular_file(status)) return outcome_error(Error::tool("'" + path.string() + "' is not a regular file"));
    const auto size = std::filesystem::file_size(path, error);
    if (error) return outcome_error(Error::tool("cannot size '" + path.string() + "': " + error.message()));
    if (size > k_max_edited_file_bytes) return outcome_error(Error::tool("file exceeds the 16 MiB edit limit: '" + path.string() + "'"));

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return outcome_error(Error::tool("cannot open '" + path.string() + "'"));
    std::string content(static_cast<usize>(size), '\0');
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<usize>(stream.gcount()));
    if (!stream && !stream.eof()) return outcome_error(Error::tool("cannot read '" + path.string() + "'"));
    if (content.find('\0') != std::string::npos) return outcome_error(Error::tool("'" + path.string() + "' looks like a binary file"));
    return content;
}

TextFile parse_text_file(std::string_view content) {
    TextFile file;
    if (content.find("\r\n") != std::string_view::npos) file.newline = "\r\n";
    file.trailing_newline = content.ends_with('\n') || content.ends_with('\r');
    auto normalized = normalize_patch(content);
    usize start = 0;
    while (start < normalized.size()) {
        const auto end = normalized.find('\n', start);
        file.lines.emplace_back(normalized.substr(start, end == std::string::npos ? normalized.size() - start : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (file.trailing_newline && !file.lines.empty() && file.lines.back().empty()) file.lines.pop_back();
    return file;
}

std::string serialize(const TextFile &file) {
    std::string content;
    for (usize index = 0; index < file.lines.size(); ++index) {
        if (index != 0) content += file.newline;
        content += file.lines[index];
    }
    if (file.trailing_newline && !file.lines.empty()) content += file.newline;
    return content;
}

std::string_view trim_left(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    return text;
}

std::string_view trim_right(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool matches_at(std::span<const std::string> lines, std::span<const std::string> sequence, usize index, i32 mode) {
    for (usize offset = 0; offset < sequence.size(); ++offset) {
        std::string_view actual = lines[index + offset];
        std::string_view expected = sequence[offset];
        if (mode >= 1) {
            actual = trim_right(actual);
            expected = trim_right(expected);
        }
        if (mode >= 2) {
            actual = trim_left(actual);
            expected = trim_left(expected);
        }
        if (actual != expected) return false;
    }
    return true;
}

std::optional<usize> find_sequence(std::span<const std::string> lines, std::span<const std::string> sequence, usize start,
                                   bool end_of_file = false) {
    if (sequence.empty()) return lines.size();
    if (sequence.size() > lines.size()) return std::nullopt;
    const auto last = lines.size() - sequence.size();
    if (start > last) return std::nullopt;
    const auto first = end_of_file ? last : start;
    for (i32 mode = 0; mode < 3; ++mode) {
        for (usize index = first; index <= last; ++index) {
            if (matches_at(lines, sequence, index, mode)) return index;
            if (end_of_file) break;
        }
    }
    return std::nullopt;
}

Result<std::string> apply_update(std::string content, const PatchOperation &operation) {
    auto file = parse_text_file(content);
    usize cursor = 0;
    for (const auto &chunk : operation.chunks) {
        if (chunk.context) {
            const std::array context_pattern{*chunk.context};
            const auto context = find_sequence(file.lines, context_pattern, cursor);
            if (!context) return outcome_error(Error::tool("context not found in '" + operation.path.string() + "': " + *chunk.context));
            cursor = *context + 1;
        }
        auto location = find_sequence(file.lines, chunk.old_lines, cursor, chunk.end_of_file);
        if (!location) return outcome_error(Error::tool("hunk did not match '" + operation.path.string() + "'"));
        auto replacement = chunk.new_lines;
        for (const auto [old_index, new_index] : chunk.context_indices) {
            replacement[new_index] = file.lines[*location + old_index];
        }
        file.lines.erase(file.lines.begin() + static_cast<isize>(*location),
                         file.lines.begin() + static_cast<isize>(*location + chunk.old_lines.size()));
        file.lines.insert(file.lines.begin() + static_cast<isize>(*location), replacement.begin(), replacement.end());
        cursor = *location + replacement.size();
    }
    return serialize(file);
}

Result<void> replace_file(const std::filesystem::path &path, std::string_view content) {
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return outcome_error(Error::tool("cannot create '" + parent.string() + "': " + error.message()));
    }

    static std::atomic<u64> nonce = 0;
    const auto temporary = path.string() + ".liminal-tmp-" + std::to_string(++nonce);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return outcome_error(Error::tool("cannot create temporary file for '" + path.string() + "'"));
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) {
            std::filesystem::remove(temporary, error);
            return outcome_error(Error::tool("cannot write temporary file for '" + path.string() + "'"));
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(std::filesystem::path(temporary).c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto message = std::system_category().message(static_cast<int>(GetLastError()));
        std::filesystem::remove(temporary, error);
        return outcome_error(Error::tool("cannot replace '" + path.string() + "': " + message));
    }
#else
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return outcome_error(Error::tool("cannot replace '" + path.string() + "': " + error.message()));
    }
#endif
    return {};
}

struct PreparedChange {
    PatchKind kind;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string content;
};

Result<std::vector<PreparedChange>> prepare_changes(const ToolSet &tools, const std::vector<PatchOperation> &operations) {
    std::map<std::filesystem::path, bool> touched;
    std::vector<PreparedChange> changes;
    changes.reserve(operations.size());
    for (const auto &operation : operations) {
        const auto source = resolve_path(tools, operation.path);
        const auto destination = operation.move_path ? resolve_path(tools, *operation.move_path) : source;
        if (touched.contains(source) || (destination != source && touched.contains(destination))) {
            return outcome_error(Error::tool("patch touches a path more than once: '" + operation.path.string() + "'"));
        }
        touched[source] = true;
        touched[destination] = true;

        if (operation.kind == PatchKind::ADD_FILE) {
            std::error_code error;
            if (std::filesystem::exists(destination, error)) {
                return outcome_error(Error::tool("cannot add existing file: '" + destination.string() + "'"));
            }
            if (error) return outcome_error(Error::tool("cannot inspect '" + destination.string() + "': " + error.message()));
            changes.push_back({.kind = operation.kind, .source = source, .destination = destination, .content = operation.added_content});
            continue;
        }

        auto original = read_text_file(source);
        if (!original) return outcome_error(std::move(original).error());
        if (operation.kind == PatchKind::DELETE_FILE) {
            changes.push_back({.kind = operation.kind, .source = source, .destination = destination});
            continue;
        }
        auto updated = apply_update(*std::move(original), operation);
        if (!updated) return outcome_error(std::move(updated).error());
        if (destination != source) {
            std::error_code error;
            if (std::filesystem::exists(destination, error)) {
                return outcome_error(Error::tool("move destination already exists: '" + destination.string() + "'"));
            }
            if (error) return outcome_error(Error::tool("cannot inspect '" + destination.string() + "': " + error.message()));
        }
        changes.push_back({.kind = operation.kind, .source = source, .destination = destination, .content = *std::move(updated)});
    }
    return changes;
}

Result<std::string> apply_changes(const std::vector<PreparedChange> &changes, const ToolSet &tools) {
    std::string result = "Done!";
    for (const auto &change : changes) {
        std::error_code error;
        if (change.kind == PatchKind::DELETE_FILE) {
            if (!std::filesystem::remove(change.source, error) || error) {
                return outcome_error(Error::tool("cannot delete '" + change.source.string() + "': " + error.message()));
            }
            const auto relative = std::filesystem::relative(change.source, tools.working_directory, error);
            result += "\nDeleted: " + (error ? change.source : relative).generic_string();
            continue;
        }
        auto written = replace_file(change.destination, change.content);
        if (!written) return outcome_error(std::move(written).error());
        if (change.destination != change.source) {
            if (!std::filesystem::remove(change.source, error) || error) {
                return outcome_error(
                    Error::tool("moved content but cannot delete old path '" + change.source.string() + "': " + error.message()));
            }
            result += "\nMoved: " + change.source.generic_string() + " -> " + change.destination.generic_string();
        } else {
            result += change.kind == PatchKind::ADD_FILE ? "\nAdded: " : "\nUpdated: ";
            const auto relative = std::filesystem::relative(change.destination, tools.working_directory, error);
            result += (error ? change.destination : relative).generic_string();
        }
    }
    return result;
}

Task<provider::ToolResult, Error> execute_apply_patch(const ToolSet &tools, const provider::ToolCall &call) {
    auto encoded = json::to_string(call.input);
    if (!encoded) co_await lighter::fail(Error::json(std::move(encoded).error(), "tool input re-encode"));
    auto parsed = json::parse<ApplyPatchInput>(*encoded);
    if (!parsed) co_await lighter::fail(Error::json(std::move(parsed).error(), "tool input"));
    auto input = *std::move(parsed);
    provider::ToolResult result{.call_id = call.id};
    auto operations = parse_patch(input.patch);
    if (!operations) {
        result.content = "Error: " + operations.error().message();
        result.is_error = true;
        co_return result;
    }
    auto changes = prepare_changes(tools, *operations);
    if (!changes) {
        result.content = "Error: " + changes.error().message();
        result.is_error = true;
        co_return result;
    }
    auto applied = apply_changes(*changes, tools);
    result.content = applied ? *std::move(applied) : "Error: " + applied.error().message();
    result.is_error = !applied;
    co_return result;
}

ToolCallPresentation describe_apply_patch(const provider::ToolCall &) { return {.description = "Apply patch"}; }

} // namespace

ToolRegistration make_apply_patch_tool() {
    return {
        .definition =
            {
                .name = "apply_patch",
                .description =
                    "Edit files with a Codex-style patch. The patch must start with '*** Begin Patch' and end with '*** End Patch'. "
                    "Use '*** Add File: path' with '+' content lines, '*** Delete File: path', or '*** Update File: path' followed by "
                    "optional '*** Move to: path' and hunks. Start each hunk with '@@' or '@@ context'; prefix removed, added, and "
                    "unchanged lines with '-', '+', and space. Paths are absolute or relative to the working directory.",
                .input_schema = {.properties = {{"patch", {.type = "string", .description = "Complete Codex-style patch text."}}},
                                 .required = {"patch"}},
            },
        .execution_mode = ToolExecutionMode::EXCLUSIVE,
        .validate = [](const provider::ToolCall &call) -> Result<void> {
            auto encoded = json::to_string(call.input);
            if (!encoded) return lighter::outcome_error(Error::json(std::move(encoded).error(), "tool input re-encode"));
            auto parsed = json::parse<ApplyPatchInput>(*encoded);
            if (!parsed) return lighter::outcome_error(Error::json(std::move(parsed).error(), "tool input"));
            return {};
        },
        .execute = execute_apply_patch,
        .describe = describe_apply_patch,
    };
}

} // namespace liminal
