#include <liminal/context/project_instructions.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <proxy/proxy.h>

#include <lighter/async/io/fs.h>
#include <lighter/async/vocab/outcome.h>
#include <lighter/encoding/utf8.h>

namespace liminal::context {

namespace {

constexpr usize k_instruction_file_limit = 1024 * 1024;

bool component_equal(const std::filesystem::path &left, const std::filesystem::path &right) {
#ifdef _WIN32
    const auto left_native = left.native();
    const auto right_native = right.native();
    return _wcsicmp(left_native.c_str(), right_native.c_str()) == 0;
#else
    return left == right;
#endif
}

bool path_within(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    while (root_part != root.end()) {
        if (candidate_part == candidate.end() || !component_equal(*root_part, *candidate_part)) {
            return false;
        }
        ++root_part;
        ++candidate_part;
    }
    return true;
}

std::string display_path(const std::filesystem::path &path) { return path.generic_string(); }

} // namespace

Result<std::filesystem::path> LocalInstructionFiles::canonicalize_instruction_path(const std::filesystem::path &path) const {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return lighter::outcome_error(
            Error::config("cannot resolve project instruction path '" + display_path(path) + "': " + error.message()));
    }
    return canonical;
}

Result<std::optional<std::string>> LocalInstructionFiles::read_instruction_file(const std::filesystem::path &path) const {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error) {
        return lighter::outcome_error(
            Error::config("cannot inspect project instruction file '" + display_path(path) + "': " + error.message()));
    }
    if (!std::filesystem::exists(status)) {
        return std::optional<std::string>{};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return lighter::outcome_error(Error::config("project instruction path is not a regular file: '" + display_path(path) + "'"));
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return lighter::outcome_error(
            Error::config("cannot size project instruction file '" + display_path(path) + "': " + error.message()));
    }
    if (size > k_instruction_file_limit) {
        return lighter::outcome_error(Error::config("project instruction file exceeds " + std::to_string(k_instruction_file_limit) +
                                                    " bytes: '" + display_path(path) + "'"));
    }

    auto content = lighter::fs::sync::read_to_string(path.string());
    if (!content) {
        return lighter::outcome_error(
            Error::config("cannot read project instruction file '" + display_path(path) + "': " + std::string(content.error().message())));
    }
    if (content->size() > k_instruction_file_limit) {
        return lighter::outcome_error(Error::config("project instruction file exceeds " + std::to_string(k_instruction_file_limit) +
                                                    " bytes: '" + display_path(path) + "'"));
    }
    if (content->find('\0') != std::string::npos) {
        return lighter::outcome_error(Error::config("project instruction file is binary: '" + display_path(path) + "'"));
    }
    if (!lighter::encoding::utf8::is_valid(*content)) {
        return lighter::outcome_error(Error::config("project instruction file is not valid UTF-8: '" + display_path(path) + "'"));
    }
    if (content->starts_with("\xEF\xBB\xBF")) {
        content->erase(0, 3);
    }
    return std::optional<std::string>{*std::move(content)};
}

Result<std::vector<InstructionSource>> ProjectInstructionResolver::resolve(const std::filesystem::path &workspace_root,
                                                                           const std::filesystem::path &active_directory,
                                                                           const InstructionFiles &files) const {
    auto canonical_root = files->canonicalize_instruction_path(workspace_root);
    if (!canonical_root) {
        return lighter::outcome_error(std::move(canonical_root).error());
    }
    auto canonical_active = files->canonicalize_instruction_path(active_directory);
    if (!canonical_active) {
        return lighter::outcome_error(std::move(canonical_active).error());
    }
    if (!path_within(*canonical_root, *canonical_active)) {
        return lighter::outcome_error(Error::config("active directory '" + display_path(*canonical_active) +
                                                    "' is outside workspace root '" + display_path(*canonical_root) + "'"));
    }

    std::vector<std::filesystem::path> directories;
    for (auto current = *canonical_active;; current = current.parent_path()) {
        directories.push_back(current);
        if (component_equal(current, *canonical_root)) {
            break;
        }
        if (current == current.parent_path()) {
            return lighter::outcome_error(Error::config("cannot traverse from active directory to workspace root"));
        }
    }
    std::ranges::reverse(directories);

    std::vector<InstructionSource> instructions;
    for (const auto &directory : directories) {
        const auto path = directory / "AGENTS.md";
        auto content = files->read_instruction_file(path);
        if (!content) {
            return lighter::outcome_error(std::move(content).error());
        }
        if (!content->has_value() || (*content)->empty()) {
            continue;
        }
        instructions.push_back({
            .authority = InstructionAuthority::PROJECT,
            .trust = InstructionTrust::WORKSPACE,
            .origin = "project:" + display_path(path),
            .scope = directory,
            .content = *std::move(*content),
        });
    }
    return instructions;
}

} // namespace liminal::context
