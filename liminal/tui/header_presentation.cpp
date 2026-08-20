#include "header_presentation.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/tui/surface.h>

namespace liminal::tui {

namespace {

constexpr std::string_view k_separator = " · ";

enum struct PathKind {
    RELATIVE,
    POSIX,
    WINDOWS_DRIVE,
    WINDOWS_UNC,
};

struct ParsedPath {
    PathKind kind = PathKind::RELATIVE;
    char separator = '/';
    std::string drive;
    std::vector<std::string> root_components;
    std::vector<std::string> components;
    bool rooted = false;
};

bool path_separator(char value, PathKind kind, char conventional) noexcept {
    if (kind == PathKind::POSIX) return value == '/';
    if (kind == PathKind::WINDOWS_DRIVE || kind == PathKind::WINDOWS_UNC || conventional == '\\') {
        return value == '/' || value == '\\';
    }
    return value == '/';
}

bool ascii_equal(std::string_view left, std::string_view right, bool insensitive) noexcept {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        auto a = static_cast<unsigned char>(left[index]);
        auto b = static_cast<unsigned char>(right[index]);
        if (insensitive) {
            a = static_cast<unsigned char>(std::tolower(a));
            b = static_cast<unsigned char>(std::tolower(b));
        }
        if (a != b) return false;
    }
    return true;
}

std::vector<std::string> split_components(std::string_view path, usize offset, PathKind kind, char conventional) {
    std::vector<std::string> result;
    while (offset < path.size()) {
        while (offset < path.size() && path_separator(path[offset], kind, conventional)) ++offset;
        const auto begin = offset;
        while (offset < path.size() && !path_separator(path[offset], kind, conventional)) ++offset;
        if (offset > begin) result.emplace_back(path.substr(begin, offset - begin));
    }
    return result;
}

ParsedPath parse_path(std::string_view raw) {
    const auto path = sanitize_terminal_text(raw);
    ParsedPath result;
    const bool drive = path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':';
    const bool unc = path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
    const bool posix = !path.empty() && path.front() == '/';
    if (posix) {
        result.separator = '/';
    } else if (drive || unc || path.contains('\\')) {
        result.separator = path.find('\\') != std::string::npos ? '\\' : '/';
    } else {
        result.separator = '/';
    }

    usize offset = 0;
    if (unc) {
        result.kind = PathKind::WINDOWS_UNC;
        result.rooted = true;
        offset = 2;
        auto components = split_components(path, offset, result.kind, result.separator);
        const auto root_count = std::min(components.size(), usize{2});
        result.root_components.assign(components.begin(), components.begin() + static_cast<isize>(root_count));
        result.components.assign(components.begin() + static_cast<isize>(root_count), components.end());
        return result;
    }
    if (drive) {
        result.kind = PathKind::WINDOWS_DRIVE;
        result.drive = path.substr(0, 2);
        offset = 2;
        result.rooted = offset < path.size() && path_separator(path[offset], result.kind, result.separator);
        result.components = split_components(path, offset, result.kind, result.separator);
        return result;
    }
    if (posix) {
        result.kind = PathKind::POSIX;
        result.rooted = true;
        offset = 1;
    } else if (path.contains('\\')) {
        result.kind = PathKind::RELATIVE;
        result.separator = '\\';
    }
    result.components = split_components(path, offset, result.kind, result.separator);
    return result;
}

bool same_root(const ParsedPath &path, const ParsedPath &home) noexcept {
    if (path.kind != home.kind || path.rooted != home.rooted) return false;
    if (path.kind == PathKind::WINDOWS_DRIVE) return ascii_equal(path.drive, home.drive, true);
    if (path.kind != PathKind::WINDOWS_UNC) return true;
    if (path.root_components.size() != home.root_components.size()) return false;
    for (usize index = 0; index < path.root_components.size(); ++index) {
        if (!ascii_equal(path.root_components[index], home.root_components[index], true)) return false;
    }
    return true;
}

bool home_contains(const ParsedPath &path, const ParsedPath &home) noexcept {
    if (!same_root(path, home) || home.components.size() > path.components.size()) return false;
    const bool insensitive = path.kind == PathKind::WINDOWS_DRIVE || path.kind == PathKind::WINDOWS_UNC || path.separator == '\\';
    for (usize index = 0; index < home.components.size(); ++index) {
        if (!ascii_equal(path.components[index], home.components[index], insensitive)) return false;
    }
    return true;
}

std::string root_text(const ParsedPath &path) {
    switch (path.kind) {
        case PathKind::POSIX: return "/";
        case PathKind::WINDOWS_DRIVE: return path.drive + (path.rooted ? std::string(1, path.separator) : std::string{});
        case PathKind::WINDOWS_UNC: {
            std::string result(2, path.separator);
            for (usize index = 0; index < path.root_components.size(); ++index) {
                if (index != 0) result += path.separator;
                result += path.root_components[index];
            }
            if (!path.root_components.empty()) result += path.separator;
            return result;
        }
        case PathKind::RELATIVE: return {};
    }
    return {};
}

std::string join_path(std::string root, const std::vector<std::string> &components, char separator) {
    for (const auto &component : components) {
        if (!root.empty() && root.back() != separator && !(root.size() == 2 && root[1] == ':')) root += separator;
        root += component;
    }
    return root;
}

struct DisplayPath {
    std::string root;
    std::vector<std::string> components;
    char separator = '/';
};

DisplayPath display_path(std::string_view path_text, const std::optional<std::string> &home_directory) {
    auto path = parse_path(path_text);
    if (home_directory && !home_directory->empty()) {
        const auto home = parse_path(*home_directory);
        if (home_contains(path, home)) {
            path.components.erase(path.components.begin(), path.components.begin() + static_cast<isize>(home.components.size()));
            return {.root = "~", .components = std::move(path.components), .separator = path.separator};
        }
    }
    return {.root = root_text(path), .components = std::move(path.components), .separator = path.separator};
}

std::string first_component_grapheme(std::string_view component) {
    usize offset = 0;
    std::string result;
    if (component.size() > 1 && component.front() == '.') {
        result = ".";
        offset = 1;
    }
    if (offset >= component.size()) return result;
    const auto grapheme = next_grapheme(component, offset);
    result.append(component.substr(grapheme.offset, grapheme.size));
    return result;
}

std::string fish_path(const DisplayPath &path) {
    if (path.components.empty()) return path.root;
    std::vector<std::string> components;
    components.reserve(path.components.size());
    for (usize index = 0; index + 1 < path.components.size(); ++index) {
        components.push_back(first_component_grapheme(path.components[index]));
    }
    components.push_back(path.components.back());
    return join_path(path.root, components, path.separator);
}

std::string truncate_cells(std::string_view text, i32 columns) {
    if (columns <= 0) return {};
    if (text_width(text) <= columns) return std::string(text);
    constexpr std::string_view ellipsis = "…";
    if (columns == 1) return std::string(ellipsis);
    std::string result;
    i32 used = 0;
    usize offset = 0;
    while (offset < text.size()) {
        const auto grapheme = next_grapheme(text, offset);
        if (used + grapheme.width > columns - 1) break;
        result.append(text.substr(grapheme.offset, grapheme.size));
        used += grapheme.width;
        offset += grapheme.size;
    }
    result += ellipsis;
    return result;
}

std::string truncated_path(const DisplayPath &path, i32 columns) {
    if (columns <= 0) return {};
    if (path.components.empty()) return truncate_cells(path.root, columns);

    const auto &final = path.components.back();
    const bool omitted = path.components.size() > 1;
    const auto direct = join_path(path.root, {final}, path.separator);
    if (omitted) {
        const auto marked = join_path(path.root, {"…", final}, path.separator);
        if (text_width(marked) <= columns) return marked;
    }
    if (text_width(direct) <= columns) return direct;

    auto prefix = path.root;
    if (omitted) {
        const auto marked_prefix = join_path(path.root, {"…", ""}, path.separator);
        if (text_width(marked_prefix) < columns) prefix = marked_prefix;
    }
    const auto prefix_width = text_width(prefix);
    if (prefix_width >= columns) return truncate_cells(prefix, columns);
    return prefix + truncate_cells(final, columns - prefix_width);
}

} // namespace

std::string normalize_header_text(std::string_view text) {
    auto safe = sanitize_terminal_text(text, true);
    std::string result;
    result.reserve(safe.size());
    bool space_pending = false;
    for (const auto value : safe) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x80 && std::isspace(byte) != 0) {
            space_pending = !result.empty();
            continue;
        }
        if (space_pending) result += ' ';
        space_pending = false;
        result += value;
    }
    return result;
}

std::string resolve_session_title(const SessionHeader &header) {
    if (header.explicit_title) {
        auto title = normalize_header_text(*header.explicit_title);
        if (!title.empty()) return title;
    }
    auto preview = normalize_header_text(header.prompt_preview);
    return preview.empty() ? "New session" : preview;
}

std::string present_workspace_path(std::string_view path, const std::optional<std::string> &home_directory, i32 columns) {
    if (columns <= 0) return {};
    const auto parsed = display_path(path.empty() ? std::string_view(".") : path, home_directory);
    auto full = join_path(parsed.root, parsed.components, parsed.separator);
    if (full.empty()) full = ".";
    if (text_width(full) <= columns) return full;
    const auto fish = fish_path(parsed);
    if (text_width(fish) <= columns) return fish;
    return truncated_path(parsed, columns);
}

std::string present_header(const HeaderContent &content, i32 columns) {
    if (columns <= 0) return {};
    auto identity = normalize_header_text(content.identity);
    if (identity.empty()) identity = "liminal";
    if (text_width(identity) >= columns) return truncate_cells(identity, columns);

    const auto identity_width = text_width(identity);
    const auto separator_width = text_width(k_separator);
    const auto content_budget = columns - identity_width - separator_width;
    if (content_budget <= 0) return truncate_cells(identity, columns);

    if (!content.include_session_title) {
        const auto workspace = present_workspace_path(content.session.workspace_path, content.session.home_directory, content_budget);
        return identity + std::string(k_separator) + workspace;
    }

    const auto title = resolve_session_title(content.session);
    if (content_budget <= separator_width + 1) {
        const auto workspace = present_workspace_path(content.session.workspace_path, content.session.home_directory, content_budget);
        return identity + std::string(k_separator) + workspace;
    }

    const auto paired_budget = content_budget - separator_width;
    const auto path_budget = paired_budget - 1;
    if (path_budget <= 0) {
        const auto workspace = present_workspace_path(content.session.workspace_path, content.session.home_directory, content_budget);
        return identity + std::string(k_separator) + workspace;
    }

    auto workspace = present_workspace_path(content.session.workspace_path, content.session.home_directory, path_budget);
    const auto title_budget = paired_budget - text_width(workspace);
    auto projected_title = truncate_cells(title, title_budget);
    if (workspace.empty() || projected_title.empty()) {
        workspace = present_workspace_path(content.session.workspace_path, content.session.home_directory, content_budget);
        return identity + std::string(k_separator) + workspace;
    }
    return identity + std::string(k_separator) + workspace + std::string(k_separator) + projected_title;
}

} // namespace liminal::tui
