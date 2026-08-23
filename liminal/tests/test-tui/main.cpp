#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/encoding/utf8.h>

#include <liminal/event.h>
#include <liminal/session/session.h>
#include <liminal/tui/clipboard.h>
#include <liminal/tui/command.h>
#include <liminal/tui/compact_picker.h>
#include <liminal/tui/compact_picker_dialog.h>
#include <liminal/tui/model_picker.h>
#include <liminal/tui/platform_paths.h>
#include <liminal/tui/headless.h>
#include <liminal/tui/external_editor.h>
#include <liminal/tui/header_presentation.h>
#include <liminal/tui/rich_text.h>
#include <liminal/tui/selectable_list_dialog.h>
#include <liminal/tui/session_commands.h>
#include <liminal/tui/session_screen.h>
#include <liminal/tui/surface.h>
#include <liminal/tui/syntax_highlight.h>

namespace {

using namespace liminal;
using namespace lighter::types;

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

std::string frame_text(const tui::Frame &frame) {
    std::string text;
    for (i32 row = 0; row < frame.surface.rows; ++row) {
        if (row != 0) text += '\n';
        text += frame.surface.row_text(row);
    }
    return text;
}

std::string styled_rows_text(const std::vector<tui::StyledRow> &rows) {
    std::string text;
    for (const auto &row : rows) {
        if (!text.empty()) text += '\n';
        for (const auto &span : row.spans) text += span.text;
    }
    return text;
}

bool has_style(const std::vector<tui::StyledRow> &rows, tui::Style style) {
    for (const auto &row : rows) {
        for (const auto &span : row.spans) {
            if (span.style == style) return true;
        }
    }
    return false;
}

bool text_has_style(const std::vector<tui::StyledRow> &rows, std::string_view text, tui::Style style) {
    for (const auto &row : rows) {
        for (const auto &span : row.spans) {
            if (span.style == style && span.text.contains(text)) return true;
        }
    }
    return false;
}

std::string layout_rows_text(const std::vector<tui::LayoutRow> &rows) {
    std::string text;
    for (const auto &row : rows) {
        if (!text.empty()) text += '\n';
        if (row.spans.empty()) {
            text += row.text;
        } else {
            for (const auto &span : row.spans) text += span.text;
        }
    }
    return text;
}

bool layout_text_has_style(const std::vector<tui::LayoutRow> &rows, std::string_view text, tui::Style style) {
    for (const auto &row : rows) {
        if (row.spans.empty()) {
            if (row.style == style && row.text.contains(text)) return true;
            continue;
        }
        for (const auto &span : row.spans) {
            if (span.style == style && span.text.contains(text)) return true;
        }
    }
    return false;
}

void check_surface_cells_and_encoding() {
    tui::Surface surface(4, 1);
    require(surface.write(0, 0, "A中B") == 4, "surface must account for wide terminal cells");
    require(surface.row_text(0) == "A中B", "continuation cells must not duplicate wide glyphs");
    require(surface.cells[2].continuation, "wide glyphs must reserve a continuation cell");
    require(tui::text_width("A中B") == 4, "text width must match surface placement");
    require(tui::text_width("👩‍💻") == 2, "emoji ZWJ sequence must occupy one wide grapheme");
    require(tui::text_width("🇨🇳") == 2, "regional-indicator pair must occupy one wide grapheme");
    require(tui::text_width("✔️") == 2, "emoji variation selector must determine cluster width");

    tui::Surface graphemes(4, 1);
    require(graphemes.write(0, 0, "👩‍💻X") == 3, "surface must place whole grapheme clusters");
    require(graphemes.cells[1].continuation, "wide grapheme must reserve its continuation cell");
    require(graphemes.row_text(0) == "👩‍💻X", "surface must retain the complete grapheme text");
    graphemes.write(0, 1, "B");
    require(!graphemes.cells[1].continuation && graphemes.row_text(0) == " BX",
            "overwriting a continuation cell must clear its complete previous grapheme");

    tui::Frame frame{.surface = std::move(surface), .cursor = {.row = 0, .column = 3, .visible = true}};
    const auto encoded = tui::encode_frame(frame);
    require(encoded.starts_with("\x1b[?25l\x1b[H"), "frame must start from a hidden cursor at terminal home");
    require(encoded.contains("\x1b[2K"), "frame must clear every owned row");
    require(encoded.ends_with("\x1b[1;4H\x1b[?25h"), "frame must restore the requested visible cursor");
    require(tui::encode_frame_diff(&frame, frame).empty(), "an unchanged frame must not emit terminal operations");

    tui::Frame transcript_palette{.surface = tui::Surface(8, 1)};
    transcript_palette.surface.write(0, 0, "T", tui::Style::ASSISTANT);
    transcript_palette.surface.write(0, 1, "S", tui::Style::EMPHASIS);
    transcript_palette.surface.write(0, 2, "I", tui::Style::ITALIC);
    transcript_palette.surface.write(0, 3, "Q", tui::Style::QUOTE);
    transcript_palette.surface.write(0, 4, "S", tui::Style::QUOTE_EMPHASIS);
    transcript_palette.surface.write(0, 5, "I", tui::Style::QUOTE_ITALIC);
    transcript_palette.surface.write(0, 6, "M", tui::Style::MUTED);
    transcript_palette.surface.write(0, 7, "L", tui::Style::LINK);
    const auto encoded_transcript = tui::encode_frame(transcript_palette);
    require(encoded_transcript.contains("\x1b[0;22;38;2;204;204;204mT"), "assistant prose must use the softer normal-weight foreground");
    require(encoded_transcript.contains("\x1b[0;1;38;2;255;255;255mS") && encoded_transcript.contains("\x1b[0;22;3;38;2;204;204;204mI"),
            "strong and italic assistant text must apply only their Markdown attributes");
    require(encoded_transcript.contains("\x1b[0;22;38;2;166;227;161mQ") && encoded_transcript.contains("\x1b[0;1;38;2;166;227;161mS") &&
                encoded_transcript.contains("\x1b[0;22;3;38;2;166;227;161mI"),
            "blockquotes must keep green normal, strong, and italic variants");
    require(encoded_transcript.contains("\x1b[0;22;38;2;166;173;200mM") && !encoded_transcript.contains("\x1b[2m"),
            "secondary text must use a stable neutral instead of terminal-dependent dim intensity");
    require(encoded_transcript.contains("\x1b[0;22;4;38;2;137;220;235mL"),
            "links must set their bright color and attributes independently of adjacent spans");

    tui::Frame palette{.surface = tui::Surface(12, 1)};
    constexpr tui::Style syntax_styles[] = {
        tui::Style::CODE,        tui::Style::CODE_BLOCK,    tui::Style::CODE_KEYWORD,  tui::Style::CODE_PREPROCESSOR,
        tui::Style::CODE_TYPE,   tui::Style::CODE_FUNCTION, tui::Style::CODE_STRING,   tui::Style::CODE_COMMENT,
        tui::Style::CODE_NUMBER, tui::Style::CODE_CONSTANT, tui::Style::CODE_PROPERTY, tui::Style::CODE_OPERATOR,
    };
    for (usize index = 0; index < std::size(syntax_styles); ++index) {
        palette.surface.write(0, static_cast<i32>(index), "K", syntax_styles[index]);
    }
    const auto encoded_palette = tui::encode_frame(palette);
    require(encoded_palette.contains("\x1b[0;1;38;2;203;166;247mK"), "code keywords must use bold, high-luminance mauve");
    require(encoded_palette.contains("\x1b[0;1;38;2;250;179;135mK"), "code preprocessors must use distinct bold, high-luminance peach");
    require(encoded_palette.contains("\x1b[0;22;38;2;205;214;244mK"), "generic block code must use a calm near-foreground tint");
    require(encoded_palette.contains("\x1b[0;22;38;2;249;226;175mK"), "inline code must keep a warm accent distinct from prose");
    require(!encoded_palette.contains("\x1b[2m") && !encoded_palette.contains("\x1b[90m"),
            "syntax highlighting must not dim text or use a dark comment color");

    tui::Frame diff_palette{.surface = tui::Surface(3, 1)};
    diff_palette.surface.write(0, 0, "+", tui::Style::DIFF_ADDITION);
    diff_palette.surface.write(0, 1, "-", tui::Style::DIFF_DELETION);
    diff_palette.surface.write(0, 2, "@", tui::Style::DIFF_HUNK);
    const auto encoded_diff = tui::encode_frame(diff_palette);
    require(encoded_diff.contains("\x1b[0;22;38;2;166;227;161m+") && encoded_diff.contains("\x1b[0;22;38;2;243;139;168m-") &&
                encoded_diff.contains("\x1b[0;1;38;2;116;199;236m@"),
            "diff styles must use fixed truecolor so brightness never depends on terminal ANSI definitions");

    tui::Frame footer_palette{.surface = tui::Surface(3, 1)};
    constexpr tui::Style footer_styles[] = {
        tui::Style::FOOTER_MODEL,
        tui::Style::FOOTER_CONTEXT,
        tui::Style::FOOTER_TOKENS,
    };
    for (usize index = 0; index < std::size(footer_styles); ++index) {
        footer_palette.surface.write(0, static_cast<i32>(index), "F", footer_styles[index]);
    }
    const auto encoded_footer = tui::encode_frame(footer_palette);
    require(encoded_footer.contains("\x1b[0;22;38;2;249;226;175mF") && encoded_footer.contains("\x1b[0;22;38;2;250;179;135mF") &&
                encoded_footer.contains("\x1b[0;22;38;2;137;180;250mF"),
            "footer metadata fields must encode three distinct high-luminance colors");

    tui::Frame shimmer_palette{.surface = tui::Surface(2, 1)};
    shimmer_palette.surface.write(0, 0, "S", tui::Style::WORKING_BASE);
    shimmer_palette.surface.write(0, 1, "S", tui::Style::WORKING_PEAK);
    const auto encoded_shimmer = tui::encode_frame(shimmer_palette);
    require(encoded_shimmer.contains("\x1b[0;22;38;2;108;112;134mS") && encoded_shimmer.contains("\x1b[0;22;38;2;205;214;244mS"),
            "working shimmer endpoints must encode a visible neutral brightness range");

    tui::Frame composer{.surface = tui::Surface(4, 1)};
    composer.surface.fill_row(0, tui::Style::COMPOSER);
    composer.surface.write(0, 0, ">", tui::Style::COMPOSER);
    require(std::ranges::all_of(composer.surface.cells, [](const tui::Cell &cell) { return cell.style == tui::Style::COMPOSER; }),
            "a filled composer row must retain its background style across every cell");
    require(tui::encode_frame(composer).contains("\x1b[0;22;39;48;2;38;38;38m>   "),
            "the composer style must encode an opaque neutral background across trailing cells");

    auto changed = frame;
    changed.surface.write(0, 3, "C");
    const auto diff = tui::encode_frame_diff(&frame, changed);
    require(diff.starts_with("\x1b[?25l\x1b[1;1H"), "a row diff must address only the changed row");
    require(!diff.contains("\x1b[H"), "a same-sized diff must not redraw from terminal home");

    tui::Surface untrusted(12, 1);
    untrusted.write(0, 0, "safe\x1b[?1049l");
    const tui::Frame safe_frame{.surface = std::move(untrusted)};
    const auto safe_encoded = tui::encode_frame(safe_frame);
    require(!safe_encoded.contains("safe\x1b[?1049l"), "untrusted text must not inject VT sequences into a frame");
    require(safe_frame.surface.row_text(0).contains("safe\xef\xbf\xbd[?1049l"),
            "terminal controls must remain visible as replacement characters");

    tui::Frame forged{.surface = tui::Surface(4, 1)};
    forged.surface.cells[0].text = "X\x1b[2J";
    require(!tui::encode_frame(forged).contains("X\x1b[2J"), "the frame encoder must defend against directly forged cell text");
    require(tui::sanitize_terminal_text("line\n\x1b[2J", true) == "line\n\xef\xbf\xbd[2J",
            "plain output must preserve layout while replacing terminal controls");
}

void check_header_identity_and_path_presentation() {
    tui::SessionHeader identity{.workspace_path = "/workspace", .explicit_title = "Explicit title", .prompt_preview = "Preview"};
    require(tui::resolve_session_title(identity) == "Explicit title", "an explicit name must win over the prompt preview");
    identity.explicit_title.reset();
    require(tui::resolve_session_title(identity) == "Preview", "the first prompt preview must identify an unnamed session");
    identity.prompt_preview.clear();
    require(tui::resolve_session_title(identity) == "New session", "an empty session must use the stable fallback title");
    identity.prompt_preview = "  first line\nsecond\tpart\r\nthird\x1b[2J  ";
    require(tui::resolve_session_title(identity) == "first line second part third�[2J",
            "preview projection must collapse layout whitespace and expose terminal controls safely");

    const auto windows = std::string("D:\\.Project\\liminal\\src");
    require(tui::present_workspace_path("D:\\", std::nullopt, 80) == "D:\\", "a Windows drive root must remain intact");
    require(tui::present_workspace_path(windows, std::nullopt, 80) == windows, "a fitting Windows drive path must remain full");
    require(tui::present_workspace_path(windows, std::nullopt, tui::text_width("D:\\.P\\l\\src")) == "D:\\.P\\l\\src",
            "a Windows drive path must use fish-like hidden and intermediate components");
    require(tui::present_workspace_path(windows, std::nullopt, tui::text_width("D:\\…\\src")) == "D:\\…\\src",
            "final truncation must preserve the drive root and final component when they fit");
    require(tui::present_workspace_path(windows, std::nullopt, 0).empty(), "a zero path budget must produce no cells");
    require(tui::present_workspace_path(windows, std::nullopt, 1) == "…", "an impossibly narrow drive path must clip deterministically");

    const auto posix = std::string("/home/alice/projects/liminal");
    const auto posix_home = std::optional<std::string>("/home/alice");
    require(tui::present_workspace_path("/", std::nullopt, 80) == "/", "a POSIX filesystem root must remain intact");
    require(tui::present_workspace_path(posix, posix_home, 80) == "~/projects/liminal",
            "a POSIX home descendant must use a tilde in its full representation");
    require(tui::present_workspace_path(posix, posix_home, tui::text_width("~/p/liminal")) == "~/p/liminal",
            "a home-relative POSIX path must retain slash separators when fish-shortened");
    require(tui::present_workspace_path("/home/alice", posix_home, 80) == "~", "the exact home directory must project to a tilde");
    require(tui::present_workspace_path("/home/alice2/project", posix_home, 80) == "/home/alice2/project",
            "home containment must compare complete path components");
    require(tui::present_workspace_path("/tmp/a\\b/project", std::nullopt, 80) == "/tmp/a\\b/project",
            "an absolute POSIX path must retain a backslash inside a legal component");
    require(tui::present_workspace_path("/tmp/project/a\\b", std::nullopt, tui::text_width("/t/p/a\\b")) == "/t/p/a\\b",
            "fish shortening must keep a final POSIX component containing a backslash and retain slash separators");
    require(tui::present_workspace_path("/home/a\\b/project", std::optional<std::string>("/home/a\\b"), 80) == "~/project",
            "POSIX home containment must treat a backslash as component data");
    require(tui::present_workspace_path("/very/long/liminal", std::nullopt, tui::text_width("/…/liminal")) == "/…/liminal",
            "a truncated POSIX path must preserve its root and fitting final component");

    const auto windows_home = std::optional<std::string>("C:\\Users\\Alice");
    require(tui::present_workspace_path("c:\\users\\alice\\Project\\liminal", windows_home, 80) == "~\\Project\\liminal",
            "Windows home containment must be component-aware and ASCII case-insensitive");
    require(tui::present_workspace_path("C:/Users/Alice/Project/liminal", std::optional<std::string>("c:/users/alice"),
                                        tui::text_width("~/P/liminal")) == "~/P/liminal",
            "drive paths written with conventional forward slashes must preserve those separators");

    const auto unicode_path = std::string("C:\\éclair\\.项目\\路径\\👩‍💻-app");
    const auto unicode_fish = std::string("C:\\é\\.项\\路\\👩‍💻-app");
    require(tui::present_workspace_path(unicode_path, std::nullopt, tui::text_width(unicode_fish)) == unicode_fish,
            "fish shortening must retain combining, hidden-wide, wide, and emoji ZWJ graphemes intact");
    const auto unicode_truncated = tui::present_workspace_path(unicode_path, std::nullopt, 10);
    require(unicode_truncated == "C:\\👩‍💻-app" && tui::text_width(unicode_truncated) <= 10 &&
                lighter::encoding::utf8::is_valid(unicode_truncated),
            "cell truncation must preserve a fitting final emoji component as complete grapheme clusters");

    tui::HeadlessSession session(80, 10);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless header action must apply"); };
    ok({.type = "set_header", .text = "/home/alice/projects/liminal", .preview = "First prompt", .home_directory = "/home/alice"});
    auto snapshot = session.inspect();
    require(snapshot.workspace_path == "/home/alice/projects/liminal" && snapshot.session_title == "First prompt" &&
                snapshot.visible_text.front() == "liminal · ~/projects/liminal · First prompt",
            "the normal headless session must retain semantic identity and render its contextual header");

    ok({.type = "set_session_title", .preview = "First prompt", .title = "Named session"});
    snapshot = session.inspect();
    require(snapshot.session_title == "Named session" && snapshot.visible_text.front().ends_with(" · Named session"),
            "an explicit rename must refresh the visible header immediately");
    ok({.type = "set_session_title", .preview = "First prompt"});
    require(session.inspect().visible_text.front().ends_with(" · First prompt"),
            "clearing an explicit name must restore the preview immediately");
    ok({.type = "set_header", .text = "D:\\other\\workspace", .preview = "Switched preview", .title = "Switched session"});
    snapshot = session.inspect();
    require(snapshot.workspace_path == "D:\\other\\workspace" && snapshot.session_title == "Switched session" &&
                snapshot.visible_text.front() == "liminal · D:\\other\\workspace · Switched session",
            "replacing semantic session identity must refresh both workspace and title");

    const auto narrow_emoji_header =
        tui::present_header({.identity = "liminal",
                             .session = {.workspace_path = "/workspace", .explicit_title = "👩‍💻 develops with élan"},
                             .include_session_title = true},
                            25);
    require(tui::text_width(narrow_emoji_header) <= 25 && lighter::encoding::utf8::is_valid(narrow_emoji_header) &&
                !narrow_emoji_header.ends_with("‍"),
            "narrow title degradation must stay cell-bounded and never split a combining or ZWJ grapheme");

    tui::HeadlessSession first_prompt(60, 8);
    require(first_prompt.apply({.type = "set_header", .text = "D:\\work"}).has_value(), "new-session identity must apply");
    require(first_prompt.inspect().visible_text.front().ends_with(" · New session"), "a new screen must show the fallback title");
    require(first_prompt.apply({.type = "submit", .text = "multiline\npreview\ttext"}).has_value(), "first prompt must submit");
    require(first_prompt.inspect().visible_text.front().ends_with(" · multiline preview text"),
            "the first submitted prompt must establish the header preview without waiting for another frame cycle");

    const auto long_prompt = std::string(239, 'a') + "👩‍💻" + std::string(4096, 'z');
    tui::HeadlessSession bounded_preview(80, 8);
    require(bounded_preview.apply({.type = "set_header", .text = "/workspace"}).has_value(), "bounded-preview identity must apply");
    require(bounded_preview.apply({.type = "submit", .text = long_prompt}).has_value(), "a large first prompt must submit");
    const auto durable_preview = session::session_preview(long_prompt);
    require(bounded_preview.screen.header.prompt_preview == durable_preview && durable_preview.size() == 239,
            "live and durable first-prompt state must use the identical 240-byte UTF-8 boundary");

    const auto unicode_native =
#ifdef _WIN32
        std::filesystem::path(u8"C:\\用户\\项目");
    constexpr std::string_view unicode_native_text = "C:\\用户\\项目";
#else
        std::filesystem::path(u8"/tmp/用户/项目");
    constexpr std::string_view unicode_native_text = "/tmp/用户/项目";
#endif
    const auto encoded_native = tui::native_path_utf8(unicode_native);
    require(encoded_native && *encoded_native == unicode_native_text && lighter::encoding::utf8::is_valid(*encoded_native),
            "native non-ASCII paths must cross the presentation boundary as valid UTF-8");
    const auto encoded_home = tui::user_home_directory_utf8();
    require(encoded_home && (!*encoded_home || lighter::encoding::utf8::is_valid(**encoded_home)),
            "the optional native home directory must cross the presentation boundary as valid UTF-8");

    tui::ConsoleRenderer state_only_renderer;
    state_only_renderer.redraw_pending = false;
    state_only_renderer.set_session_title("Persisted title", "Preview");
    require(!state_only_renderer.redraw_pending && state_only_renderer.screen.header.explicit_title == "Persisted title",
            "session title synchronization must mutate screen state without creating a pre-persistence redraw");

    tui::HeadlessSession resume(80, 10);
    resume.selectable_list.emplace("unused", "No sessions", tui::SelectableListPage{});
    resume.selectable_list->set_contextual_header(
        {.identity = "Resume Session", .session = {.workspace_path = posix, .home_directory = posix_home}});
    require(resume.inspect().visible_text.front() == "Resume Session · ~/projects/liminal",
            "the resume list must render its workspace-aware contextual header");

    tui::HeadlessSession history(80, 10);
    history.selectable_list.emplace("unused", "No checkpoints", tui::SelectableListPage{});
    history.selectable_list->set_contextual_header(
        {.identity = "Conversation History",
         .session = {.workspace_path = posix, .home_directory = posix_home, .explicit_title = "Named session"},
         .include_session_title = true});
    require(history.inspect().visible_text.front() == "Conversation History · ~/projects/liminal · Named session",
            "the conversation-history list must render workspace and session identity");

    tui::HeadlessSession browsing(36, 8);
    require(browsing.apply({.type = "set_header", .text = posix, .preview = "Resize title", .home_directory = posix_home}).has_value(),
            "browsing identity must apply");
    for (i32 index = 0; index < 10; ++index) {
        require(browsing.apply({.type = "notice", .text = "line-" + std::to_string(index)}).has_value(),
                "browsing fixture must append transcript rows");
    }
    require(browsing.apply({.type = "page_up"}).has_value(), "browsing fixture must leave the tail");
    const auto anchor = browsing.inspect().anchor;
    require(anchor.has_value(), "PageUp must establish a semantic anchor before resize");
    require(browsing.apply({.type = "resize", .columns = 24, .rows = 8}).has_value(), "browsing resize must apply");
    const auto resized = browsing.inspect();
    require(resized.anchor == anchor && tui::text_width(resized.visible_text.front()) <= resized.columns,
            "header reprojection on resize must preserve the browsing anchor and visible-cell bounds");
    require(browsing.apply({.type = "follow_tail"}).has_value(), "the fixture must return to tail following");
    const auto tail_anchor = browsing.inspect().anchor;
    require(!tail_anchor && browsing.apply({.type = "resize", .columns = 48, .rows = 8}).has_value() && !browsing.inspect().anchor,
            "tail-following resize must remain at the tail while refreshing the header");
}

void check_composer_editing() {
    tui::Composer composer;
    composer.insert("ac");
    composer.move_left();
    composer.insert("b");
    require(composer.text == "abc" && composer.cursor == 2, "composer must insert at its cursor");
    composer.erase();
    composer.backspace();
    require(composer.text == "a" && composer.cursor == 1, "delete and backspace must edit around the cursor");

    composer.insert("中");
    composer.move_left();
    composer.move_right();
    require(composer.cursor == composer.text.size(), "cursor movement must preserve grapheme boundaries");
    composer.insert("👩‍💻");
    composer.move_left();
    require(composer.cursor == std::string("a中").size(), "cursor movement must treat an emoji sequence as one grapheme");
    composer.erase();
    require(composer.text == "a中", "delete must remove a complete grapheme cluster");
    composer.insert("\nnext");
    require(composer.take() == "a中\nnext", "pasted newlines must remain part of one submitted prompt");
    require(composer.text.empty() && composer.cursor == 0, "submission must reset composer state");

    composer.insert("discard me");
    composer.move_left();
    composer.clear();
    require(composer.empty() && composer.cursor == 0, "clearing must reset composer text and cursor");
}

void check_multiline_navigation_history_and_projection() {
    tui::Composer composer;
    composer.replace("one two\n中x\nz");
    composer.move_document_home();
    composer.move_word_right();
    require(composer.cursor == 3, "word-right must stop after the current word");
    composer.move_word_right();
    require(composer.cursor == 7, "word-right must cross whitespace to the next word boundary");
    composer.move_end();
    require(composer.cursor == 7, "End must target the current logical line");
    require(composer.move_down() && composer.cursor == std::string("one two\n中x").size(),
            "down must preserve the display-cell column and clamp to a shorter line");
    require(composer.move_down() && composer.cursor == composer.text.size(), "down must reach the final logical line");
    require(composer.move_up() && composer.cursor == std::string("one two\n中x").size(),
            "vertical navigation must preserve its preferred display-cell column");
    composer.backspace_word();
    require(composer.text == "one two\n\nz", "word backspace must remove one Unicode word without crossing a newline");

    tui::SessionScreen screen;
    screen.resize({20, 9});
    screen.set_model("test", std::nullopt);
    screen.insert("first");
    require(screen.take_prompt() == "first", "submission must return the current prompt");
    screen.insert("second\nline");
    require(screen.take_prompt() == "second\nline", "later submissions must remain independent");
    screen.insert("scratch");
    screen.move_up();
    require(screen.composer.text == "second\nline", "Up at a draft boundary must recall the newest prompt");
    screen.move_up();
    require(screen.composer.text == "first", "repeated Up must walk older history instead of moving inside a recalled prompt");
    screen.move_down();
    require(screen.composer.text == "second\nline", "Down while browsing history must walk toward newer prompts");
    screen.move_down();
    require(screen.composer.text == "scratch", "Down past the newest history entry must restore the original draft");

    screen.replace_prompt("top\nbottom");
    screen.move_up();
    require(screen.composer.text == "top\nbottom" && screen.composer.cursor == 3,
            "Up must retain natural cursor movement when a multiline draft has an adjacent line");
    screen.move_down();
    require(screen.composer.cursor == screen.composer.text.size(),
            "Down must retain natural cursor movement when a multiline draft has an adjacent line");

    tui::SessionScreen visual;
    visual.resize({40, 10});
    visual.set_model("test-model", std::optional<std::string>("high"));
    visual.set_header({.workspace_path = "D:\\code\\liminal"});
    visual.set_footer({.context_left_percent = 54, .tokens_used = 1'184'000});
    const auto empty = visual.frame();
    const auto row_uses_style = [&empty](i32 row, tui::Style style) {
        const auto begin = empty.surface.cells.begin() + static_cast<isize>(row * empty.surface.columns);
        return std::ranges::all_of(begin, begin + empty.surface.columns, [style](const tui::Cell &cell) { return cell.style == style; });
    };
    require(empty.surface.row_text(0) == "liminal · D:\\code\\liminal · New session",
            "the header must combine product, workspace, and resolved session title");
    require(empty.surface.row_text(6).empty() && empty.surface.row_text(7) == "›" && empty.surface.row_text(8).empty() &&
                row_uses_style(6, tui::Style::COMPOSER) && row_uses_style(7, tui::Style::COMPOSER) &&
                row_uses_style(8, tui::Style::COMPOSER),
            "an empty composer must render as a padded three-row input surface");
    require(empty.cursor.row == 7 && empty.cursor.column == 2, "the composer cursor must begin after its concise prompt marker");
    require(empty.surface.row_text(9) == "test-model high · Context 54% left" &&
                empty.surface.cells[static_cast<usize>(9 * empty.surface.columns)].style == tui::Style::FOOTER_MODEL,
            "the footer must remove token usage as a whole when all metadata does not fit");

    visual.resize({80, 10});
    const auto wide_footer = visual.frame();
    const auto footer_row = static_cast<usize>(9 * wide_footer.surface.columns);
    const auto footer_style = [&wide_footer, footer_row](std::string_view text, tui::Style style) {
        const auto row = wide_footer.surface.row_text(9);
        const auto byte_offset = row.find(text);
        if (byte_offset == std::string::npos) return false;
        return wide_footer.surface.cells[footer_row + static_cast<usize>(tui::text_width(row.substr(0, byte_offset)))].style == style;
    };
    require(wide_footer.surface.row_text(9) == "test-model high · Context 54% left · 1.18M used",
            "the footer must format the remaining three session metadata items compactly");
    require(footer_style("test-model high", tui::Style::FOOTER_MODEL) && footer_style("Context 54% left", tui::Style::FOOTER_CONTEXT) &&
                footer_style("1.18M used", tui::Style::FOOTER_TOKENS),
            "each footer metadata item must use its own semantic color");
    require(!wide_footer.surface.row_text(9).contains("D:\\code\\liminal"), "the workspace must no longer have a footer rendering path");

    visual.footer.not_saving = true;
    visual.resize({20, 10});
    const auto degraded_footer = visual.frame();
    require(degraded_footer.surface.row_text(9) == "SESSION NOT SAVING" &&
                degraded_footer.surface.cells[static_cast<usize>(9 * degraded_footer.surface.columns)].style == tui::Style::FAILURE,
            "a narrow degraded persistence queue must replace ordinary metadata with its critical warning");

    screen.clear_prompt();
    screen.insert("one\ntwo\nthree\nfour");
    const auto frame = screen.frame();
    require(screen.viewport_rows() == 4, "a growing composer must retain transcript viewport space");
    require(frame.surface.row_text(5) == "  two" && frame.surface.row_text(6) == "  three" && frame.surface.row_text(7) == "  four",
            "an overflowing composer must vertically window around its cursor");
    require(frame.cursor.row == 7 && frame.cursor.column == 6, "the multiline composer cursor must remain visible on its logical row");

    tui::SessionScreen active;
    active.apply(AssistantTextDelta{.text = "streaming"});
    active.insert("queued draft");
    require(active.state == tui::SessionState::STREAMING, "editing a draft during a task must not overwrite the task's semantic state");

    active.external_editor_active = true;
    active.resize({40, 8});
    require(frame_text(active.frame()).contains("Save and close external editor"),
            "the status row must explain how to return from an external editor handoff");
}

void check_responsive_footer() {
    tui::SessionScreen screen;
    screen.resize({80, 6});
    screen.set_model("model", std::optional<std::string>("high"));
    screen.set_footer({
        .context_left_percent = 54,
        .tokens_used = 1'184'000,
        .provider_limits = "Limit 80%",
    });
    const auto footer_at = [&screen](i32 columns) {
        screen.resize({columns, 6});
        return screen.frame().surface.row_text(5);
    };

    const auto full = std::string("model high · Context 54% left · 1.18M used · Limit 80%");
    const auto without_tokens = std::string("model high · Context 54% left · Limit 80%");
    const auto model_and_limits = std::string("model high · Limit 80%");
    const auto model_only = std::string("model high");
    const auto full_width = tui::text_width(full);
    const auto without_tokens_width = tui::text_width(without_tokens);
    const auto model_and_limits_width = tui::text_width(model_and_limits);
    const auto model_width = tui::text_width(model_only);

    require(footer_at(full_width) == full, "a wide footer must retain model, context, tokens, and provider limits in semantic order");
    require(footer_at(full_width - 1) == without_tokens && footer_at(without_tokens_width) == without_tokens,
            "the first oversized boundary must remove the complete token segment and retain exact fits");
    require(footer_at(without_tokens_width - 1) == model_and_limits && footer_at(model_and_limits_width) == model_and_limits,
            "the next oversized boundary must remove context while retaining provider limits");
    require(footer_at(model_and_limits_width - 1) == model_only && footer_at(model_width) == model_only,
            "provider limits must disappear only after tokens and context, leaving the complete model at its exact width");
    require(footer_at(model_width - 1) == "model hi…",
            "model plus effort may truncate only after every other ordinary footer segment is gone");

    for (i32 width = 1; width <= full_width; ++width) {
        const auto projected = footer_at(width);
        require(!projected.starts_with(" · ") && !projected.ends_with(" · ") && !projected.contains(" ·  · "),
                "responsive footer projection must never leave an orphan separator");
        require(tui::text_width(projected) <= width, "every responsive footer projection must fit its terminal-cell budget");
    }

    screen.set_model("A👩‍💻éZ", std::optional<std::string>("👨‍👩‍👧‍👦X"));
    require(footer_at(8) == "A👩‍💻éZ …" && lighter::encoding::utf8::is_valid(footer_at(8)),
            "final model truncation must preserve combining and ZWJ extended grapheme clusters");

    screen.set_model("model", std::optional<std::string>("high"));
    screen.set_footer({.context_left_percent = 54, .tokens_used = 1'184'000});
    require(footer_at(80) == "model high · Context 54% left · 1.18M used",
            "an absent provider limit must leave no text, spacing, or separator residue");
}

void check_rich_output_and_concurrent_tools() {
    constexpr std::string_view fixture = R"md(# Rich output
Paragraph with **strong**, *emphasis*, `inline code`, and [docs](https://example.com/docs).
The foo_bar_baz identifier stays literal.
> Quoted **strong words**, *italic words*, `inline quote code`, and [reference](https://example.com/quote) with enough words to wrap.
- first list item with enough words to wrap cleanly
1. ordered item
```cpp
if (ready) {
    run();
    ++count;
    const auto message = "ready"; // note
    Widget result = build_widget(config.value, MAX_RETRIES + 1);
    return 42;
}
```
```diff
diff --git a/file.cpp b/file.cpp
@@ -1 +1 @@
-old value
+new value
```
)md";

    const auto wide = tui::layout_rich_text(fixture, 72);
    const auto narrow = tui::layout_rich_text(fixture, 24);
    const auto wide_text = styled_rows_text(wide);
    const auto narrow_text = styled_rows_text(narrow);
    require(narrow.size() > wide.size(), "narrow rich output must wrap into more deterministic rows");
    for (const auto &row : narrow) {
        std::string text;
        for (const auto &span : row.spans) text += span.text;
        require(tui::text_width(text) <= 24, "every rich row must fit its terminal width");
    }
    require(wide_text.starts_with("Rich output") && !wide_text.contains("assistant: ") && !wide_text.contains("# Rich output"),
            "headings must render without a role prefix or their Markdown marker");
    require(text_has_style(wide, "Paragraph with ", tui::Style::ASSISTANT) &&
                text_has_style(wide, "first list item", tui::Style::ASSISTANT),
            "ordinary assistant prose and list bodies must use the soft neutral transcript style");
    require(wide_text.contains("strong") && !wide_text.contains("**strong**") && has_style(wide, tui::Style::EMPHASIS) &&
                has_style(wide, tui::Style::ITALIC),
            "strong and emphasis markup must become terminal styles");
    require(wide_text.contains("foo_bar_baz"), "intraword underscores in code-like identifiers must remain literal");
    require(wide_text.contains("docs") && wide_text.contains("<https://example.com/docs>") && has_style(wide, tui::Style::LINK),
            "links must retain a visible, styled target");
    require(wide_text.contains("│ Quoted") && !wide_text.contains("> Quoted") && text_has_style(wide, "│ Quoted ", tui::Style::QUOTE) &&
                text_has_style(wide, "strong words", tui::Style::QUOTE_EMPHASIS) &&
                text_has_style(wide, "italic words", tui::Style::QUOTE_ITALIC) &&
                text_has_style(wide, "inline quote code", tui::Style::CODE) && text_has_style(wide, "reference", tui::Style::LINK),
            "blockquotes must render green prose with contextual inline Markdown styles");
    require(narrow_text.contains("\n│ "), "wrapped blockquotes must retain their green quote gutter");
    require(wide_text.contains("• first list item") && narrow_text.contains("  enough words"),
            "lists must retain a bullet and continuation indentation when wrapped");
    require(wide_text.contains("┌ cpp") && wide_text.contains("│     run();") && wide_text.contains("└") &&
                text_has_style(wide, "count", tui::Style::NORMAL) && !text_has_style(wide, "++", tui::Style::DIFF_ADDITION),
            "fenced code must use a quiet gutter, retain whitespace, and keep ordinary code out of diff styling");
    require(text_has_style(wide, "const", tui::Style::CODE_KEYWORD) && text_has_style(wide, "\"ready\"", tui::Style::CODE_STRING) &&
                text_has_style(wide, "// note", tui::Style::CODE_COMMENT) && text_has_style(wide, "42", tui::Style::CODE_NUMBER),
            "recognized fenced languages must style keywords, strings, comments, and numbers semantically");
    require(text_has_style(wide, "Widget", tui::Style::CODE_TYPE) && text_has_style(wide, "build_widget", tui::Style::CODE_FUNCTION) &&
                text_has_style(wide, "MAX_RETRIES", tui::Style::CODE_CONSTANT) &&
                text_has_style(wide, "value", tui::Style::CODE_PROPERTY) && text_has_style(wide, "+", tui::Style::CODE_OPERATOR),
            "recognized fenced languages must distinguish richer semantic token roles");
    const auto preprocessor = tui::layout_rich_text("```cpp\n#include <vector>\n```", 50);
    require(text_has_style(preprocessor, "#include", tui::Style::CODE_PREPROCESSOR),
            "C++ preprocessor directives must use the dedicated bright syntax style");
    require(has_style(wide, tui::Style::DIFF_ADDITION) && has_style(wide, tui::Style::DIFF_DELETION) &&
                has_style(wide, tui::Style::DIFF_HUNK),
            "unified diff markers must receive distinct semantic styles");

    const auto nested_fence = tui::layout_rich_text("````markdown\n```nested```\n````", 40);
    const auto nested_text = styled_rows_text(nested_fence);
    require(nested_text.starts_with("┌ markdown") && nested_text.contains("│ ```nested```") && nested_text.ends_with("└"),
            "a longer Markdown fence must retain shorter fence sequences as code");

    const auto multiline_comment = tui::layout_rich_text("```cpp\n/* first\nstill */ return 7;\n```", 40);
    require(text_has_style(multiline_comment, "still */", tui::Style::CODE_COMMENT) &&
                text_has_style(multiline_comment, "return", tui::Style::CODE_KEYWORD),
            "syntax state must carry across code lines and resume highlighting after a block comment");
    const auto unknown_language = tui::layout_rich_text("```unknown\nif value == 7\n```", 40);
    require(text_has_style(unknown_language, "if value == 7", tui::Style::CODE_BLOCK),
            "unknown fenced languages must fall back to the generic block code style");
    const auto python = tui::layout_rich_text("```Python3\ndef greet(name=\"Ada\"): # welcome\n    return 1\n```", 50);
    require(text_has_style(python, "def", tui::Style::CODE_KEYWORD) && text_has_style(python, "\"Ada\"", tui::Style::CODE_STRING) &&
                text_has_style(python, "# welcome", tui::Style::CODE_COMMENT) && text_has_style(python, "1", tui::Style::CODE_NUMBER),
            "language aliases must select the matching lightweight lexer family");
    const auto typescript =
        tui::layout_rich_text("```typescript\nconst render = (value: number) => service.format(`value=${value}`);\n```", 70);
    require(text_has_style(typescript, "const", tui::Style::CODE_KEYWORD) && text_has_style(typescript, "number", tui::Style::CODE_TYPE) &&
                text_has_style(typescript, "format", tui::Style::CODE_FUNCTION) &&
                text_has_style(typescript, "value=", tui::Style::CODE_STRING),
            "new registry-backed lexer families must render without TUI-specific language routing");
    const auto json = tui::layout_rich_text("```json\n{\"name\": \"liminal\", \"ready\": true}\n```", 50);
    require(text_has_style(json, "\"name\"", tui::Style::CODE_PROPERTY) && text_has_style(json, "\"liminal\"", tui::Style::CODE_STRING) &&
                text_has_style(json, "true", tui::Style::CODE_CONSTANT),
            "newly registered data languages must render semantic token roles");

    tui::CodeHighlighter guarded("cpp");
    const auto oversized = guarded.highlight_line(std::string(4097, 'x'));
    require(!guarded.supported() && oversized.size() == 1 && oversized.front().style == tui::Style::CODE_BLOCK,
            "oversized source lines must disable highlighting and retain generic block code output");

    tui::SessionScreen streaming;
    streaming.resize({40, 16});
    streaming.apply(AssistantTextDelta{.text = "Partial **strong"});
    auto partial = streaming.layout_block(streaming.transcript.blocks.back());
    std::string partial_text;
    for (const auto &row : partial) {
        for (const auto &span : row.spans) partial_text += span.text;
    }
    require(partial_text.contains("**strong"), "incomplete streaming markup must remain literal");
    streaming.apply(AssistantTextDelta{.text = "**"});
    streaming.apply(AssistantMessageCompleted{});
    streaming.layout_block(streaming.transcript.blocks.back());
    streaming.layout_block(streaming.transcript.blocks.back());
    const auto diagnostics = streaming.layout_diagnostics();
    require(diagnostics.cache_hits > 0 && diagnostics.cached_blocks == 1,
            "a completed rich block must enter the existing stable layout cache");

    auto now = std::chrono::steady_clock::time_point{};
    tui::SessionScreen tools([&now] { return now; });
    tools.apply(ToolStarted{.call_id = "one", .name = "read_file", .description = "Read README.md"});
#ifdef _WIN32
    const auto command = std::string(R"(rg -n "needle" . --glob '!build/**' | Select-Object -First 10)");
#else
    const auto command = std::string(R"(rg -n "needle" . --glob '!build/**' | head -n 10)");
#endif
    tools.apply(ToolStarted{.call_id = "two", .name = "exec_command", .command = command});
    tools.apply(ToolCompleted{
        .call_id = "one",
        .name = "read_file",
        .description = "Read README.md",
        .summary = "5 lines · 189 bytes",
    });
    require(tools.state == tui::SessionState::RUNNING_TOOLS, "one completed tool must not hide a concurrently running sibling");
    const auto read_rows = tools.layout_block(tools.transcript.blocks[0]);
    std::string read_text;
    for (const auto &row : read_rows) read_text += row.text + "\n";
    require(read_text.contains("✓ Read README.md") && read_text.contains("└ 5 lines · 189 bytes"),
            "completed tools must show the concrete action and bounded output summary");
    require(read_rows.size() == 2 && read_rows[0].style == tui::Style::NORMAL && read_rows[1].style == tui::Style::MUTED,
            "tool actions and targets must stay bright while completion detail is muted");

    auto command_rows = tools.layout_block(tools.transcript.blocks[1]);
    require(layout_rows_text(command_rows).contains("• Running " + command),
            "a running shell tool must render its command inline after the Running state");
    require(layout_text_has_style(command_rows, "rg", tui::Style::CODE_FUNCTION),
            "shell executables must be highlighted in command position");
    require(layout_text_has_style(command_rows, "-n", tui::Style::CODE_PROPERTY) &&
                layout_text_has_style(command_rows, "--glob", tui::Style::CODE_PROPERTY),
            "shell options must be visually distinct from ordinary arguments");
    require(layout_text_has_style(command_rows, "needle", tui::Style::CODE_STRING) &&
                layout_text_has_style(command_rows, "|", tui::Style::CODE_OPERATOR),
            "shell strings and pipeline operators must retain their semantic styles");
#ifdef _WIN32
    require(layout_text_has_style(command_rows, "Select-Object", tui::Style::CODE_FUNCTION) &&
                layout_text_has_style(command_rows, "-First", tui::Style::CODE_PROPERTY),
            "Windows command pipelines must use PowerShell command and option semantics");
#else
    require(layout_text_has_style(command_rows, "head", tui::Style::CODE_FUNCTION),
            "Linux command pipelines must use Bash command semantics");
#endif
    now += std::chrono::seconds(9);
    require(!layout_rows_text(tools.layout_block(tools.transcript.blocks[1])).contains("(9s)"),
            "short-running commands must not show elapsed time");
    now += std::chrono::seconds(2);
    require(tools.has_elapsed_running_command() && layout_rows_text(tools.layout_block(tools.transcript.blocks[1])).contains("(11s)"),
            "commands running beyond ten seconds must show elapsed time");

    tools.apply(ToolCompleted{
        .call_id = "two",
        .name = "exec_command",
        .command = command,
        .summary = "exit 0 · stdout 2 lines\nstdout:\n2 tests passed",
    });
    command_rows = tools.layout_block(tools.transcript.blocks[1]);
    const auto completed_command = layout_rows_text(command_rows);
    require(completed_command.contains("✓ Ran " + command) && !completed_command.contains("(11s)"),
            "a completed shell tool must switch to Ran and stop showing live elapsed time");
    require(layout_text_has_style(command_rows, "exit 0", tui::Style::MUTED) &&
                layout_text_has_style(command_rows, "2 tests passed", tui::Style::MUTED),
            "command status and output preview rows must use the dim secondary style");
    require(tools.state == tui::SessionState::WAITING,
            "after the final concurrent tool completes, a task without active text must return to provider waiting");
}

void check_external_editor_round_trip(std::string_view executable) {
    auto parsed = tui::parse_external_editor_command("code --wait \"profile name\"");
    require(parsed.has_value() && parsed->arguments == std::vector<std::string>{"code", "--wait", "profile name"},
            "external editor commands must preserve quoted platform-native arguments");

    tui::ExternalEditorCommand command{
        .arguments = {std::filesystem::absolute(executable).string(), "--external-editor-helper"},
    };
    lighter::EventLoop loop;
    auto task = tui::run_external_editor("original draft", command);
    loop.schedule(task);
    loop.run();
    auto edited = task.result();
    require(edited.has_value() && *edited == "edited externally\n\n",
            "external editor handoff must seed a temporary file and reload its saved contents");

#ifdef _WIN32
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-editor-test-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    const auto batch = directory / "editor helper.cmd";
    {
        std::ofstream output(batch, std::ios::binary | std::ios::trunc);
        output << "@echo off\r\n>\"%~1\" echo edited by batch\r\n";
        require(static_cast<bool>(output), "failed to create the Windows batch editor fixture");
    }
    tui::ExternalEditorCommand batch_command{.arguments = {batch.string()}};
    auto batch_task = tui::run_external_editor("batch seed", batch_command);
    loop.schedule(batch_task);
    loop.run();
    auto batch_edited = batch_task.result();
    require(batch_edited.has_value() && batch_edited->starts_with("edited by batch"),
            "Windows editor handoff must launch .cmd shims such as code.cmd through COMSPEC");
    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
#endif
}

void check_clipboard_helper() {
#ifndef _WIN32
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("liminal-clipboard-test-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    const auto helper = directory / "wl-copy";
    const auto output_path = directory / "clipboard.txt";
    {
        std::ofstream output(helper, std::ios::binary | std::ios::trunc);
        output << "#!/bin/sh\ncat > \"$LIMINAL_CLIPBOARD_TEST_OUTPUT\"\n";
        require(static_cast<bool>(output), "failed to create the clipboard helper fixture");
    }
    std::filesystem::permissions(helper, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

    const auto *old_path_pointer = std::getenv("PATH");
    const std::string old_path = old_path_pointer ? old_path_pointer : "";
    const auto test_path = directory.string() + ":" + old_path;
    setenv("PATH", test_path.c_str(), 1);
    setenv("WAYLAND_DISPLAY", "liminal-test", 1);
    setenv("LIMINAL_CLIPBOARD_TEST_OUTPUT", output_path.c_str(), 1);

    lighter::EventLoop loop;
    auto task = tui::copy_to_clipboard("copied from Liminal\n");
    loop.schedule(task);
    loop.run();
    const auto copied = task.result();

    setenv("PATH", old_path.c_str(), 1);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("LIMINAL_CLIPBOARD_TEST_OUTPUT");

    std::ifstream input(output_path, std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(input), {});
    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    require(copied.has_value() && contents == "copied from Liminal\n",
            "Linux clipboard integration must stream the reply to a session-compatible helper");
#endif
}

void check_command_parsing_and_status() {
    auto input = tui::parse_repl_input("ordinary prompt");
    const auto *prompt = input ? std::get_if<tui::UserPrompt>(&*input) : nullptr;
    require(prompt && prompt->text == "ordinary prompt", "ordinary input must remain a user prompt");

    input = tui::parse_repl_input("/copy  3\t");
    const auto *copy = input ? std::get_if<tui::CommandLine>(&*input) : nullptr;
    require(copy && copy->name == "copy" && copy->arguments == "3", "slash input must split its exact name and arguments");
    auto command = tui::resolve_command(copy->name);
    require(command && *command == tui::CommandKind::COPY, "the copy command must resolve centrally");

    const auto newest = tui::parse_copy_arguments("");
    const auto older = tui::parse_copy_arguments("  3\t");
    const auto zero = tui::parse_copy_arguments("0");
    const auto trailing = tui::parse_copy_arguments("2 more");
    require(newest && newest->ordinal == 1, "/copy must select the newest reply");
    require(older && older->ordinal == 3, "/copy N must use a newest-first positive ordinal");
    require(!zero && !trailing && zero.error().detail.contains("usage"), "malformed copy ordinals must report the command usage");

    input = tui::parse_repl_input("/copycat");
    const auto *unknown = input ? std::get_if<tui::CommandLine>(&*input) : nullptr;
    require(unknown != nullptr, "an unknown slash command must remain command input");
    auto unresolved = tui::resolve_command(unknown->name);
    require(!unresolved && unresolved.error().kind == ErrorKind::COMMAND,
            "an unknown slash command must fail through the command boundary");
    input = tui::parse_repl_input("//copycat");
    prompt = input ? std::get_if<tui::UserPrompt>(&*input) : nullptr;
    require(prompt && prompt->text == "/copycat", "a double slash must escape a slash-prefixed user prompt");
    auto empty_command = tui::parse_repl_input("/");
    auto whitespace_command = tui::parse_repl_input("/ arguments");
    require(!empty_command && !whitespace_command && empty_command.error().kind == ErrorKind::COMMAND,
            "empty slash commands must fail through the command boundary");

    command = tui::resolve_command("exit");
    require(command && *command == tui::CommandKind::QUIT, "command aliases must be registered centrally");
    const auto resume = tui::resolve_command("resume");
    const auto fresh = tui::resolve_command("new");
    const auto archive = tui::resolve_command("archive");
    const auto unarchive = tui::resolve_command("unarchive");
    const auto history = tui::resolve_command("history");
    require(resume && *resume == tui::CommandKind::RESUME && fresh && *fresh == tui::CommandKind::NEW && !archive && !unarchive &&
                history && *history == tui::CommandKind::HISTORY,
            "session commands and removed archive commands must resolve through the central command registry");
    const auto named = tui::parse_name_arguments("  Catalog work  ");
    const auto cleared = tui::parse_name_arguments("--clear");
    require(named && named->title == "Catalog work" && cleared && !cleared->title && !tui::parse_name_arguments(""),
            "session naming arguments must distinguish a title, explicit clearing, and missing input");
    require(tui::require_no_arguments("quit", " ") && !tui::require_no_arguments("quit", "now"),
            "argument-free commands must reject unexpected arguments");

    const auto registry = tui::command_registry();
    std::set<tui::CommandKind> kinds;
    std::set<std::string_view, std::less<>> names;
    for (const auto &spec : registry) {
        kinds.insert(spec.kind);
        require(names.insert(spec.name).second, "canonical command names must be unique in the registry");
        for (const auto &alias : spec.aliases) {
            require(names.insert(alias).second, "command aliases must not collide with other registry names");
        }
        require(!spec.description.empty(), "every registry entry must describe itself");
        for (const auto &name_or_alias : names) {
            require(std::ranges::none_of(name_or_alias, [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }),
                    "registry names must stay lowercase");
        }
        auto resolved = tui::resolve_command(spec.name);
        require(resolved && *resolved == spec.kind, "every registry entry must resolve to its own kind");
    }
    require(kinds.size() == registry.size(), "every executable command kind must appear exactly once in the registry");
    require(!names.contains("archive") && !names.contains("unarchive"), "removed archive commands must stay absent from the registry");
    const auto *help = tui::find_command("help");
    require(help != nullptr && help->kind == tui::CommandKind::HELP, "/help must be registered");
    require(tui::find_command("exit") == tui::find_command("quit") && tui::find_command("exit") != nullptr,
            "aliases must find the same registry entry as the canonical name");
    require(tui::find_command("copycat") == nullptr, "unknown names must not find a registry entry");

    tui::SessionScreen screen;
    screen.resize({80, 10});
    screen.set_model("test-model", std::nullopt);
    screen.footer.not_saving = true;
    screen.show_status("Copied latest reply to clipboard");
    const auto copied = screen.frame();
    require(frame_text(copied).contains("Copied latest reply to clipboard") &&
                copied.surface.cells[static_cast<usize>(9 * copied.surface.columns)].style == tui::Style::ACCENT,
            "copy success must temporarily replace persistence and ordinary footer metadata with an emphasized confirmation");

    screen.footer.not_saving = false;
    screen.insert("next prompt");
    const auto editing = frame_text(screen.frame());
    require(!editing.contains("Copied latest reply") && editing.contains("test-model"),
            "the copy confirmation must yield to normal footer metadata on the next interaction");

    screen.show_status("Copied latest reply to clipboard");
    screen.footer.not_saving = true;
    screen.external_editor_active = true;
    const auto editor = screen.frame();
    require(frame_text(editor).contains("Save and close external editor") &&
                editor.surface.cells[static_cast<usize>(9 * editor.surface.columns)].style == tui::Style::MUTED,
            "external-editor guidance must take priority over a copy confirmation");
}

void check_help_notice_lists_registry() {
    const auto reference = tui::describe_commands();
    require(reference.starts_with("commands:\n"), "/help must open with the command reference heading");
    for (const auto &spec : tui::command_registry()) {
        require(reference.contains("/" + std::string(spec.name)), "/help must list every registered command");
        require(reference.contains(spec.description), "/help must carry every registry description");
        for (const auto &alias : spec.aliases) {
            require(reference.contains("(also /" + std::string(alias) + ")"), "/help must surface registered aliases");
        }
        if (spec.idle_only) {
            const auto line_start = reference.find("/" + std::string(spec.name));
            const auto line_end = reference.find('\n', line_start);
            require(reference.substr(line_start, line_end - line_start).contains("(idle only)"),
                    "/help must mark idle-only commands on their own line");
        }
    }
    require(reference.contains("[selector]") && reference.contains("<title> | --clear"),
            "/help must show argument synopses from the registry");
    require(reference.contains("type // to send a prompt that starts with /"), "/help must explain the double-slash escape");
    const auto lines = static_cast<usize>(std::ranges::count(reference, '\n'));
    require(lines == tui::command_registry().size() + 2, "/help must render exactly one line per command plus chrome");
}

void check_compact_picker_filtering_and_states() {
    auto item = [](std::string id, std::string primary) {
        tui::CompactPickerItem entry{.id = std::move(id), .primary = std::move(primary)};
        entry.haystacks.push_back(entry.id);
        return entry;
    };

    tui::CompactPicker prefix{.match = tui::CompactPickerMatch::PREFIX};
    std::vector<tui::CompactPickerItem> commands;
    commands.push_back(item("model", "/model"));
    commands.push_back(item("compact", "/compact"));
    commands.push_back(item("copy", "/copy"));
    prefix.set_items(std::move(commands));
    require(prefix.filtered.size() == 3, "an empty query must match every item");
    prefix.set_query("co");
    require(prefix.filtered.size() == 2 && prefix.highlighted_id() == "compact",
            "prefix matching must keep only names beginning with the query");
    prefix.set_query("ompac");
    require(prefix.filtered.empty() && !prefix.highlighted_id(), "prefix matching must not match interior text");
    prefix.set_query("CO");
    require(prefix.filtered.size() == 2, "matching must be case-insensitive");

    tui::CompactPicker picker{.query_label = "Model", .empty_message = "No matching model"};
    std::vector<tui::CompactPickerItem> models;
    for (auto name : {"alpha/one", "alpha/two", "beta/one", "beta/two", "gamma/one"}) {
        models.push_back(item(name, name));
    }
    models[3].current = true;
    picker.set_items(std::move(models));
    require(picker.filtered.size() == 5, "substring picker must start unfiltered");
    picker.move(3);
    require(picker.highlighted_id() == "beta/two", "movement must follow filtered order");
    picker.move(10);
    require(picker.highlighted_id() == "gamma/one", "movement must clamp at the last result");
    picker.move(-1);
    picker.set_query("two");
    require(picker.filtered.size() == 2 && picker.highlighted_id() == "beta/two",
            "narrowing the query must preserve the highlighted identity when it still matches");
    picker.set_query("one");
    require(picker.highlighted_id() == "alpha/one", "a highlight filtered out must reset to the first result");
    picker.set_query("");
    require(picker.highlighted_id() == "alpha/one" && picker.filtered.size() == 5,
            "clearing the query must restore the complete ordered dataset");

    require(picker.desired_rows(false) == 5 && picker.desired_rows(true) == 6, "desired rows must count results plus the owned query row");
    picker.error = "refresh failed";
    require(picker.desired_rows(true) == 7, "an error must occupy one extra row");
    picker.error.reset();

    tui::Surface surface(40, 10);
    picker.set_query("one");
    const auto cursor = picker.project(surface, 2, picker.desired_rows(true), true);
    require(surface.row_text(2) == "› alpha/one" && surface.row_text(3) == "  beta/one" && surface.row_text(4) == "  gamma/one",
            "the highlighted result must carry the selection marker");
    require(surface.row_text(5) == "Model: one", "the owned query row must render nearest the composer");
    require(cursor && cursor->row == 5 && cursor->column == tui::text_width("Model: one") && cursor->visible,
            "the query cursor must sit after the query text");

    tui::Surface narrow_query_surface(12, 1);
    picker.set_query("ab中cd👩‍💻ef");
    const auto end_cursor = picker.project(narrow_query_surface, 0, 1, true);
    require(narrow_query_surface.row_text(0) == "Model: 👩‍💻ef" && end_cursor && end_cursor->column == 11,
            "a long query must show a grapheme-safe suffix with its end cursor visible");
    picker.edit_query(tui::PickerQueryEdit::HOME);
    narrow_query_surface.clear();
    const auto home_cursor = picker.project(narrow_query_surface, 0, 1, true);
    require(narrow_query_surface.row_text(0) == "Model: ab中c" && home_cursor && home_cursor->column == tui::text_width("Model: "),
            "moving to the start must window a long query back to its grapheme-safe prefix");
    picker.edit_query(tui::PickerQueryEdit::RIGHT);
    picker.edit_query(tui::PickerQueryEdit::RIGHT);
    picker.edit_query(tui::PickerQueryEdit::RIGHT);
    picker.edit_query(tui::PickerQueryEdit::RIGHT);
    narrow_query_surface.clear();
    const auto middle_cursor = picker.project(narrow_query_surface, 0, 1, true);
    require(narrow_query_surface.row_text(0) == "Model: b中cd" && middle_cursor && middle_cursor->column == 11,
            "query windowing must follow a moved cursor without splitting wide graphemes");

    surface.clear();
    picker.set_query("one");
    picker.move(2);
    picker.project(surface, 0, 2, false);
    require(surface.row_text(0) == "  beta/one" && surface.row_text(1) == "› gamma/one",
            "a shrunken band must keep the highlighted result visible");

    surface.clear();
    picker.error = "Cannot refresh models";
    picker.set_query("nothing-matches");
    picker.project(surface, 0, picker.desired_rows(true), true);
    require(surface.row_text(0) == "No matching model" && surface.row_text(1) == "Cannot refresh models" &&
                surface.row_text(2) == "Model: nothing-matches",
            "empty and error states must render as explicit compact rows");

    surface.clear();
    picker.error.reset();
    picker.loading = true;
    picker.project(surface, 0, picker.desired_rows(false), false);
    require(surface.row_text(0) == "Loading…", "the loading state must render one compact row");

    tui::CompactPicker current_marker;
    current_marker.set_items({item("beta/two", "beta/two")});
    current_marker.items[0].current = true;
    tui::Surface marker_surface(40, 1);
    current_marker.project(marker_surface, 0, 1, false);
    require(marker_surface.row_text(0) == "› beta/two · current", "the current item must carry a visible marker");
}

void check_command_menu_activation_boundary() {
    tui::HeadlessSession session(80, 24);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless action must apply"); };

    ok({.type = "insert", .text = "/"});
    auto snapshot = session.inspect();
    require(snapshot.command_menu_open, "typing a lone slash must open the command menu");
    require(snapshot.picker_visible_ids.size() == tui::command_registry().size(), "an empty command token must list the whole registry");

    ok({.type = "insert", .text = "mo"});
    snapshot = session.inspect();
    require(snapshot.command_menu_open && snapshot.picker_visible_ids == std::vector<std::string>{"model"},
            "prefix matching must narrow the menu to the single matching command");

    ok({.type = "insert", .text = " high"});
    require(!session.inspect().command_menu_open, "a cursor inside command arguments must close the menu");
    for (i32 step = 0; step < 5; ++step) ok({.type = "left"});
    require(session.inspect().command_menu_open, "returning the cursor to the command token must reopen the menu");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "//model"});
    require(!session.inspect().command_menu_open, "an escaped double-slash draft must never open the menu");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "see /model docs"});
    require(!session.inspect().command_menu_open, "a slash later in an ordinary prompt must not open the menu");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "/name foo\nbar"});
    require(!session.inspect().command_menu_open, "a multi-line command draft with the cursor in arguments must stay closed");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "/mo"});
    require(session.inspect().command_menu_open, "a fresh command token must open the menu");
    ok({.type = "escape"});
    snapshot = session.inspect();
    require(!snapshot.command_menu_open && snapshot.composer_text == "/mo", "Esc must close the menu without modifying the draft");
    ok({.type = "left"});
    ok({.type = "right"});
    require(!session.inspect().command_menu_open, "cursor movement over an unchanged dismissed token must not reopen the menu");
    ok({.type = "insert", .text = "d"});
    require(session.inspect().command_menu_open, "editing the token must clear the dismissal and reopen the menu");
}

void check_command_menu_key_semantics() {
    tui::HeadlessSession session(80, 24);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless action must apply"); };

    ok({.type = "insert", .text = "/mo"});
    auto snapshot = session.inspect();
    require(snapshot.command_menu_open && snapshot.picker_highlight_id == "model", "the prefix match must be highlighted");
    ok({.type = "tab"});
    snapshot = session.inspect();
    require(snapshot.composer_text == "/model" && snapshot.composer_cursor == 6 && snapshot.command_menu_open,
            "Tab must complete the canonical command name and keep the menu open");
    require(snapshot.blocks.empty(), "Tab completion must not submit anything");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "/hi"});
    ok({.type = "submit"});
    snapshot = session.inspect();
    require(snapshot.composer_text == "/history" && snapshot.blocks.empty(),
            "Enter on an incomplete highlighted command must complete it without executing");
    ok({.type = "submit"});
    snapshot = session.inspect();
    require(snapshot.blocks.size() == 1 && snapshot.composer_text.empty() && !snapshot.command_menu_open,
            "Enter on an exact command must fall through to normal submission");

    ok({.type = "insert", .text = "/exit"});
    require(session.inspect().command_menu_open, "an alias token must keep the menu open");
    ok({.type = "submit"});
    snapshot = session.inspect();
    require(snapshot.blocks.size() == 2 && snapshot.composer_text.empty(),
            "Enter on an exact alias must fall through to normal submission");

    ok({.type = "insert", .text = "/c"});
    snapshot = session.inspect();
    require(snapshot.picker_visible_ids == std::vector<std::string>{"context", "compact", "copy"},
            "prefix results must keep registry order");
    require(snapshot.picker_highlight_id == "context", "the first result must be highlighted initially");
    ok({.type = "down"});
    require(session.inspect().picker_highlight_id == "compact", "Down must move the highlight");
    ok({.type = "down"});
    ok({.type = "down"});
    require(session.inspect().picker_highlight_id == "copy", "the highlight must clamp at the last result");
    ok({.type = "up"});
    require(session.inspect().picker_highlight_id == "compact", "Up must move the highlight back");
    require(session.inspect().composer_text == "/c", "moving the highlight must not modify the draft");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "/copycat"});
    snapshot = session.inspect();
    require(snapshot.command_menu_open && snapshot.picker_visible_ids.empty() && !snapshot.picker_highlight_id,
            "an unknown token must show the explicit empty state");
    ok({.type = "submit"});
    snapshot = session.inspect();
    require(snapshot.blocks.size() == 3 && snapshot.composer_text.empty(),
            "Enter with no matching command must submit into the command error boundary");

    ok({.type = "insert", .text = "hello"});
    ok({.type = "tab"});
    require(session.inspect().composer_text == "hello\t", "Tab outside an active menu must keep inserting a literal tab");
}

void check_command_menu_rows_and_availability() {
    tui::HeadlessSession session(100, 24);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless action must apply"); };

    ok({.type = "insert", .text = "/re"});
    auto snapshot = session.inspect();
    bool found = false;
    for (const auto &row : snapshot.visible_text) {
        if (!row.contains("/resume")) continue;
        found = true;
        require(row.contains("› /resume"), "the highlighted row must carry the selection marker");
        require(row.contains("switch to another session") && row.contains("(idle only)"),
                "menu rows must show the description and availability without hiding the command");
    }
    require(found, "the matching command row must be visible above the composer");

    ok({.type = "clear"});
    ok({.type = "insert", .text = "/copy"});
    snapshot = session.inspect();
    found = false;
    for (const auto &row : snapshot.visible_text) {
        if (!row.contains("[reply number]")) continue;
        found = true;
        require(row.contains("/copy") && row.contains("copy an assistant reply"),
                "menu rows must include the argument synopsis alongside the description");
    }
    require(found, "the synopsis row must be visible");

    const auto before_rows = session.inspect();
    ok({.type = "escape"});
    const auto after_rows = session.inspect();
    require(!after_rows.command_menu_open, "Esc must close the menu");
    require(after_rows.composer_text == before_rows.composer_text, "closing the menu must preserve the draft");
}

void check_empty_session_hint() {
    tui::HeadlessSession session(80, 12);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless action must apply"); };
    auto shows_hint = [&] {
        const auto snapshot = session.inspect();
        return std::ranges::any_of(snapshot.visible_text,
                                   [](const std::string &row) { return row.contains("Ask Liminal anything. Type / for commands."); });
    };

    require(shows_hint(), "a fresh empty session must show the discovery hint");
    ok({.type = "insert", .text = "/"});
    require(!shows_hint(), "the open command menu must replace the hint");
    ok({.type = "escape"});
    require(shows_hint(), "closing the menu on an empty session must restore the hint");
    ok({.type = "resize", .columns = 60, .rows = 10});
    require(shows_hint(), "resize must not lose the hint on an empty session");
    ok({.type = "notice", .text = "session restored"});
    require(!shows_hint(), "any transcript content must retire the hint");
}

void check_scroll_resize_state() {
    tui::SessionScreen screen;
    screen.resize({18, 8});
    screen.set_model("test-model", std::optional<std::string>("high"));
    for (i32 index = 0; index < 9; ++index) {
        screen.apply(SessionNotice{.text = "line-" + std::to_string(index)});
    }

    const auto tail = screen.frame();
    require(frame_text(tail).contains("line-8"), "following viewport must show transcript tail");
    require(tail.surface.row_text(0).starts_with("liminal ·"), "the responsive header must retain recognizable product identity");

    screen.move_up();
    require(!screen.anchor.has_value(), "Up at a composer boundary must not scroll the transcript");
    screen.move_down();
    require(!screen.anchor.has_value(), "Down at a composer boundary must not scroll the transcript");

    screen.page(-1);
    require(screen.anchor.has_value(), "PageUp must establish a semantic viewport anchor");
    const auto original_anchor = *screen.anchor;
    const auto history = screen.frame();
    require(history.surface.row_text(7).starts_with("test-model high"), "scrolling back must preserve normal footer metadata");
    require(!frame_text(history).contains("line-8"), "PageUp must move away from the transcript tail");

    screen.apply(SessionNotice{.text = "line-9"});
    require(screen.anchor == original_anchor, "new output must preserve a browsing anchor");
    require(screen.frame().surface.row_text(7).starts_with("test-model high"),
            "new output while browsing must not replace footer metadata");

    screen.resize({10, 8});
    require(screen.anchor == original_anchor, "resize must preserve the semantic source anchor");
    const auto resized = screen.frame();
    require(resized.surface.row_text(7) == "test-mode…",
            "resize must deterministically retain and cell-truncate the model after dropping other footer metadata");
    require(tui::encode_frame(resized) == tui::encode_frame(screen.frame()), "unchanged state must produce an identical frame");
    const auto diagnostics = screen.layout_diagnostics();
    require(diagnostics.cache_hits > 0 && diagnostics.cached_blocks > 0, "repeated frames must reuse stable block layout");

    screen.follow_tail();
    require(!screen.anchor, "returning to tail must clear the browsing anchor");
    require(frame_text(screen.frame()).contains("line-9"), "returning to tail must reveal the newest output");
}

void check_repeated_activity_ids_are_provider_scoped() {
    constexpr ActivityScope first_call{.task_generation = 19, .provider_call_generation = 41};
    constexpr ActivityScope second_call{.task_generation = 19, .provider_call_generation = 42};
    tui::SessionScreen screen;
    screen.resize({48, 12});
    screen.apply(PromptSubmitted{.text = "repeat IDs"});
    screen.apply(AssistantTextDelta{.item_id = "output:0", .text = "checking", .activity_scope = first_call});
    screen.apply(AssistantMessageCompleted{
        .item_id = "output:0",
        .text = "checking",
        .phase = provider::MessagePhase::COMMENTARY,
        .activity_scope = first_call,
    });
    const auto completed_message_blocks = screen.transcript.blocks.size();
    screen.apply(AssistantTextDelta{.item_id = "output:0", .text = "late", .activity_scope = first_call});
    require(screen.state == tui::SessionState::WAITING && screen.transcript.blocks.size() == completed_message_blocks &&
                screen.transcript.blocks.back().text == "checking",
            "a late delta must not reactivate an item already settled in the current provider-call generation");
    screen.apply(ToolStarted{.call_id = "call", .name = "read_file", .activity_scope = first_call});
    screen.apply(ToolCompleted{.call_id = "call", .name = "read_file", .summary = "done", .activity_scope = first_call});
    const auto completed_tool_blocks = screen.transcript.blocks.size();
    screen.apply(ToolStarted{.call_id = "call", .name = "read_file", .activity_scope = first_call});
    require(screen.state == tui::SessionState::WAITING && screen.transcript.blocks.size() == completed_tool_blocks &&
                screen.active_tool_calls.empty(),
            "a late start must not reactivate a tool already settled in the current provider-call generation");
    screen.apply(ProviderActivityCompleted{.activity_scope = first_call});

    screen.apply(AssistantTextDelta{.item_id = "output:0", .text = "final", .activity_scope = second_call});
    require(screen.state == tui::SessionState::STREAMING && frame_text(screen.frame()).contains("Writing…") &&
                screen.transcript.blocks.back().text == "final" && screen.transcript.blocks.back().activity_scope == second_call,
            "a repeated provider-local output ID must start visible Writing activity in the next provider-call generation");
    const auto block_count = screen.transcript.blocks.size();
    screen.apply(AssistantMessageCompleted{
        .item_id = "output:0",
        .text = "stale",
        .phase = provider::MessagePhase::COMMENTARY,
        .activity_scope = first_call,
    });
    require(screen.state == tui::SessionState::STREAMING && screen.transcript.blocks.size() == block_count &&
                screen.transcript.blocks.back().text == "final",
            "a late completion from the retired provider-call generation must not settle the repeated current identity");
    screen.apply(AssistantMessageCompleted{
        .item_id = "output:0",
        .text = "final answer",
        .phase = provider::MessagePhase::FINAL,
        .activity_scope = second_call,
    });
    require(screen.state == tui::SessionState::WAITING && screen.transcript.blocks.back().text == "final answer",
            "the matching current generation must complete the repeated identity and return activity to Thinking");
}

void check_activity_state() {
    auto now = std::chrono::steady_clock::time_point{};
    tui::SessionScreen screen([&now] { return now; });
    screen.resize({40, 10});
    screen.set_model("test-model", std::nullopt);
    const auto idle_rows = screen.viewport_rows();
    require(!screen.task_active() && screen.activity_rows().empty(), "an idle session must not show task activity");

    const auto activity_text = [](const tui::SessionScreen &target) { return layout_rows_text(target.activity_rows()); };
    const auto shimmer_style = [](tui::Style style) {
        switch (style) {
            case tui::Style::WORKING_BASE:
            case tui::Style::WORKING_LOW:
            case tui::Style::WORKING_MEDIUM:
            case tui::Style::WORKING_HIGH:
            case tui::Style::WORKING_BRIGHT:
            case tui::Style::WORKING_PEAK: return true;
            default: return false;
        }
    };
    const auto require_shimmer = [&](std::string_view label) {
        const auto rows = screen.activity_rows();
        require(rows.size() == 2 && layout_rows_text({rows.front()}) == label,
                "the activity projector must preserve the complete current semantic label");
        require(
            std::ranges::all_of(rows.front().spans, [&](const tui::StyledSpan &span) { return shimmer_style(span.style); }) &&
                std::ranges::any_of(rows.front().spans, [](const tui::StyledSpan &span) { return span.style == tui::Style::WORKING_PEAK; }),
            "every active semantic label must retain the full-label shimmer");
    };
    const auto require_elapsed = [&](std::string_view label) {
        const auto rows = screen.activity_rows();
        require(layout_rows_text({rows.front()}) == std::string(label) + " (3s)" && rows.front().spans.back().style == tui::Style::MUTED,
                "every active semantic label must retain the muted elapsed suffix after three seconds");
    };

    screen.apply(PromptSubmitted{.text = "go"});
    require(screen.task_active() && screen.animating() && activity_text(screen).starts_with("• Thinking…"),
            "a submitted prompt must enter the animated provider-waiting state");
    require(screen.viewport_rows() == idle_rows, "in-flow activity must not shrink the transcript viewport");
    const auto waiting = screen.frame();
    require(waiting.surface.row_text(1) == "go" && waiting.surface.row_text(2).contains("Thinking…") && waiting.surface.row_text(3).empty(),
            "a waiting task must show the unprefixed prompt above Thinking and leave space before the composer");
    require(screen.activity_rows().front().spans.size() == 1 &&
                screen.activity_rows().front().spans.front().style == tui::Style::WORKING_BASE,
            "activity shimmer must begin at its base brightness");

    now += std::chrono::milliseconds(900);
    require_shimmer("• Thinking…");
    const auto highlighted = screen.activity_rows().front().spans;
    now += std::chrono::milliseconds(100);
    require(screen.activity_rows().front().spans != highlighted,
            "the full-label shimmer must advance on each shared 100ms animation frame");

    screen.apply(AssistantTextDelta{.item_id = "message-1", .text = "hi"});
    require(screen.state == tui::SessionState::STREAMING && activity_text(screen).starts_with("• Writing…"),
            "visible assistant text must select Writing");
    require_shimmer("• Writing…");

    screen.apply(AssistantMessageCompleted{.item_id = "message-1", .text = "hi"});
    require(screen.state == tui::SessionState::WAITING && activity_text(screen).starts_with("• Thinking…"),
            "completing an assistant message must return an otherwise active task to Thinking");

    screen.apply(ToolStarted{.call_id = "tool-1", .name = "read_file", .description = "Read one"});
    require(screen.active_tool_calls.size() == 1 && activity_text(screen).starts_with("• Running tool…"),
            "one active tool identity must select the singular running label");
    require_shimmer("• Running tool…");
    screen.apply(ToolStarted{.call_id = "tool-2", .name = "read_file", .description = "Read two"});
    require(screen.active_tool_calls.size() == 2 && activity_text(screen).starts_with("• Running 2 tools…"),
            "two active tool identities must select the counted running label");
    require_shimmer("• Running 2 tools…");

    screen.apply(AssistantTextDelta{.item_id = "message-2", .text = "overlap"});
    require(screen.streaming_assistant_items.size() == 1 && activity_text(screen).starts_with("• Running 2 tools…"),
            "assistant deltas during tool execution must not hide tool activity");
    screen.apply(ToolCompleted{.call_id = "tool-1", .name = "read_file", .description = "Read one", .summary = "done"});
    require(screen.active_tool_calls.size() == 1 && activity_text(screen).starts_with("• Running tool…"),
            "completing one concurrent tool must decrement rather than hide its running sibling");
    screen.apply(ToolCompleted{.call_id = "tool-2", .name = "read_file", .description = "Read two", .summary = "done"});
    require(screen.active_tool_calls.empty() && activity_text(screen).starts_with("• Writing…"),
            "completing the final tool must reveal still-active visible assistant streaming");
    screen.apply(AssistantMessageCompleted{.item_id = "message-2", .text = "overlap"});
    require(screen.streaming_assistant_items.empty() && activity_text(screen).starts_with("• Thinking…"),
            "completing the overlapping message must return the active task to Thinking");

    now += std::chrono::milliseconds(100);
    now += std::chrono::seconds(2);
    require_elapsed("• Thinking…");
    constexpr ActivityScope final_scope{.task_generation = 1, .provider_call_generation = 2};
    screen.apply(AssistantTextDelta{.item_id = "message-3", .text = "more", .activity_scope = final_scope});
    require_elapsed("• Writing…");
    screen.apply(ToolStarted{
        .call_id = "tool-3",
        .name = "exec_command",
        .command = "echo hi",
        .activity_scope = final_scope,
    });
    require_elapsed("• Running tool…");
    screen.apply(ToolStarted{
        .call_id = "tool-4",
        .name = "read_file",
        .description = "Read more",
        .activity_scope = final_scope,
    });
    require_elapsed("• Running 2 tools…");

    for (i32 index = 0; index < 12; ++index) screen.apply(SessionNotice{.text = "line-" + std::to_string(index)});
    require(frame_text(screen.frame()).contains("Running 2 tools…"), "a full viewport must keep activity at the transcript tail");
    screen.page(-1);
    require(screen.anchor.has_value() && !frame_text(screen.frame()).contains("Running 2 tools…"),
            "browsing history must scroll activity away with the flow");
    screen.follow_tail();
    require(frame_text(screen.frame()).contains("Running 2 tools…"), "returning to the tail must reveal activity again");

    screen.apply(ToolCompleted{.call_id = "tool-3", .name = "exec_command", .summary = "done", .activity_scope = final_scope});
    screen.apply(ToolCompleted{.call_id = "tool-4", .name = "read_file", .summary = "done", .activity_scope = final_scope});
    screen.apply(AssistantMessageCompleted{.item_id = "message-3", .text = "more", .activity_scope = final_scope});
    screen.apply(ProviderActivityCompleted{.activity_scope = final_scope});
    screen.apply(TaskCompleted{.task_generation = final_scope.task_generation});
    require(!screen.task_active() && !screen.animating() && screen.active_tool_calls.empty() && screen.streaming_assistant_items.empty(),
            "a completed task must clear every active lifecycle identity");
    const auto completed = screen.frame();
    require(frame_text(completed).contains("Finished (3s)"), "a completed task must retain its final elapsed status");
    i32 finished_row = -1;
    for (i32 row = 0; row < completed.surface.rows; ++row) {
        if (completed.surface.row_text(row).contains("Finished")) finished_row = row;
    }
    require(finished_row >= 0, "the completed frame must contain a finished status row");
    require(completed.surface.cells[static_cast<usize>(finished_row * completed.surface.columns)].style == tui::Style::MUTED,
            "the finished status must use the same muted color as tool output details");
    require(finished_row + 1 < completed.surface.rows && completed.surface.row_text(finished_row + 1).empty(),
            "the finished status must leave space before the composer");
    screen.insert("next");
    require(frame_text(screen.frame()).contains("Finished (3s)"), "editing the next prompt must preserve the finished status");
    screen.apply(PromptSubmitted{.text = "next"});
    require(!frame_text(screen.frame()).contains("Finished") && frame_text(screen.frame()).contains("Thinking…") &&
                screen.active_tool_calls.empty() && screen.streaming_assistant_items.empty(),
            "a subsequent prompt must replace Finished with Thinking and begin with clean lifecycle identities");
    screen.apply(AssistantTextDelta{.item_id = "message-3", .text = "late", .activity_scope = final_scope});
    screen.apply(ToolStarted{.call_id = "tool-3", .name = "exec_command", .command = "late", .activity_scope = final_scope});
    require(activity_text(screen).starts_with("• Thinking…") && screen.active_tool_calls.empty() &&
                screen.streaming_assistant_items.empty(),
            "late identities from the prior task must not contaminate a subsequent prompt");

    tui::SessionScreen cancelled;
    constexpr ActivityScope cancelled_scope{.task_generation = 2, .provider_call_generation = 1};
    cancelled.apply(PromptSubmitted{.text = "cancel"});
    cancelled.apply(AssistantTextDelta{.item_id = "cancel-message", .text = "partial", .activity_scope = cancelled_scope});
    cancelled.apply(ToolStarted{.call_id = "cancel-tool", .name = "read_file", .activity_scope = cancelled_scope});
    cancelled.apply(TaskCancelled{});
    const auto cancelled_blocks = cancelled.transcript.blocks.size();
    cancelled.apply(AssistantMessageCompleted{.item_id = "cancel-message", .text = "late", .activity_scope = cancelled_scope});
    cancelled.apply(ToolCompleted{.call_id = "cancel-tool", .name = "read_file", .summary = "late", .activity_scope = cancelled_scope});
    require(cancelled.state == tui::SessionState::CANCELLED && cancelled.activity_rows().empty() && cancelled.active_tool_calls.empty() &&
                cancelled.streaming_assistant_items.empty() && cancelled.transcript.blocks.size() == cancelled_blocks &&
                !frame_text(cancelled.frame()).contains("Finished"),
            "cancellation and late completions must clear activity without retaining a finished summary");

    tui::SessionScreen failed;
    constexpr ActivityScope failed_scope{.task_generation = 3, .provider_call_generation = 1};
    failed.apply(PromptSubmitted{.text = "fail"});
    failed.apply(AssistantTextDelta{.item_id = "failed-message", .text = "partial", .activity_scope = failed_scope});
    failed.apply(ToolStarted{.call_id = "failed-tool", .name = "read_file", .activity_scope = failed_scope});
    failed.apply(TaskFailed{.message = "provider failed"});
    failed.apply(AssistantTextDelta{.item_id = "failed-message", .text = "late", .activity_scope = failed_scope});
    failed.apply(ToolCompleted{.call_id = "failed-tool", .name = "read_file", .summary = "late", .activity_scope = failed_scope});
    require(failed.state == tui::SessionState::FAILED && failed.activity_rows().empty() && failed.active_tool_calls.empty() &&
                failed.streaming_assistant_items.empty() && !frame_text(failed.frame()).contains("Finished"),
            "failure and late lifecycle events must clear activity without retaining a finished summary");

    tui::SessionScreen replaced;
    constexpr ActivityScope replaced_scope{.task_generation = 4, .provider_call_generation = 1};
    replaced.apply(PromptSubmitted{.text = "replace"});
    replaced.apply(AssistantTextDelta{.item_id = "replace-message", .text = "partial", .activity_scope = replaced_scope});
    replaced.apply(ToolStarted{.call_id = "replace-tool", .name = "read_file", .activity_scope = replaced_scope});
    replaced.load_transcript({});
    replaced.apply(AssistantTextDelta{.item_id = "replace-message", .text = "late", .activity_scope = replaced_scope});
    replaced.apply(AssistantMessageCompleted{.item_id = "replace-message", .text = "late", .activity_scope = replaced_scope});
    replaced.apply(ToolStarted{.call_id = "replace-tool", .name = "read_file", .activity_scope = replaced_scope});
    replaced.apply(ToolCompleted{.call_id = "replace-tool", .name = "read_file", .summary = "late", .activity_scope = replaced_scope});
    require(replaced.state == tui::SessionState::EDITING && replaced.transcript.blocks.empty() && replaced.active_tool_calls.empty() &&
                replaced.streaming_assistant_items.empty(),
            "transcript replacement must retire stale assistant-item and tool-call identities");

    tui::SessionScreen compact([&now] { return now; });
    compact.resize({40, 4});
    compact.apply(PromptSubmitted{.text = "go"});
    require(frame_text(compact.frame()).contains("Thinking…"), "a one-row transcript viewport must drop the separator instead of activity");
}

void check_mouse_selection() {
    const auto cell_selected = [](const tui::Frame &frame, i32 row, i32 column) {
        return frame.surface.cells[static_cast<usize>(row * frame.surface.columns + column)].selected;
    };

    tui::SessionScreen screen;
    screen.resize({20, 8});
    screen.set_model("test", std::nullopt);
    screen.apply(SessionNotice{.text = "alpha beta"});
    screen.apply(SessionNotice{.text = "gamma delta"});

    screen.begin_selection(1, 0);
    require(!cell_selected(screen.frame(), 1, 0), "a selection must stay invisible until the drag moves off its press cell");
    require(screen.extend_selection(2, 4), "extending a selection must move its focus");
    require(!screen.extend_selection(2, 4), "extending to the current focus must report no change");
    const auto highlighted = screen.frame();
    require(cell_selected(highlighted, 1, 0) && cell_selected(highlighted, 1, 19) && cell_selected(highlighted, 2, 4),
            "a multi-row selection must cover the full first row tail and the focus cell");
    require(!cell_selected(highlighted, 2, 5) && !cell_selected(highlighted, 0, 0),
            "a selection must not spill past its focus cell or onto other rows");
    require(screen.take_selection() == "alpha beta\ngamma",
            "taking a selection must join per-row text with newlines and trim trailing blanks");
    require(!screen.selection && screen.take_selection().empty(), "taking a selection must clear it");

    screen.begin_selection(1, 2);
    require(!screen.has_selection() && screen.take_selection().empty(), "a click without a drag must not produce clipboard text");

    screen.begin_selection(2, 3);
    screen.extend_selection(1, 5);
    require(screen.has_selection(), "a drag that moved off its press cell must report an active selection");
    require(screen.take_selection() == " beta\ngamm", "an upward drag must normalize into reading order");

    screen.begin_selection(1, 0);
    screen.extend_selection(1, 4);
    screen.apply(SessionNotice{.text = "new content"});
    require(!screen.has_selection(), "new transcript content must invalidate a cell-anchored selection");
    screen.begin_selection(1, 0);
    screen.extend_selection(1, 4);
    screen.insert("draft");
    require(!screen.has_selection(), "composer edits must invalidate the selection because rows may reflow");
    screen.clear_prompt();
    screen.begin_selection(1, 0);
    screen.extend_selection(1, 4);
    screen.scroll(-1);
    require(!screen.has_selection(), "scrolling must invalidate the selection because content moves under it");

    tui::SessionScreen wide;
    wide.resize({10, 8});
    wide.apply(SessionNotice{.text = "中文"});
    wide.begin_selection(1, 0);
    wide.extend_selection(1, 3);
    require(wide.take_selection() == "中文", "wide graphemes must be extracted once across their continuation cells");

    tui::Frame inverted{.surface = tui::Surface(3, 1)};
    inverted.surface.write(0, 0, "abc");
    inverted.surface.cells[1].selected = true;
    const auto encoded = tui::encode_frame(inverted);
    require(encoded.contains("\x1b[7m") && encoded.contains("\x1b[27m"),
            "selected cells must toggle reverse video on and off around the selection");
}

void check_headless_virtual_time_and_snapshots() {
    tui::HeadlessSession session(24, 8, 100);
    require(session.render_count == 1, "headless creation must capture an initial frame");
    require(session.apply({.type = "insert", .text = "hello"}).has_value(), "headless input action must apply");
    require(session.apply({.type = "submit"}).has_value(), "headless submit action must apply");
    require(session.apply({.type = "assistant_delta", .text = "world"}).has_value(), "headless response action must apply");
    require(session.render_pending && session.render_count == 1, "same-tick actions must coalesce before the frame deadline");
    require(session.apply({.type = "advance_time", .milliseconds = 15}).has_value(), "virtual time must advance");
    require(session.render_count == 1, "a frame must remain pending before its virtual deadline");
    require(session.apply({.type = "advance_time", .milliseconds = 1}).has_value(), "virtual time must reach the deadline");
    const auto snapshot = session.inspect();
    require(!snapshot.render_pending && snapshot.render_count == 2, "deadline must flush one coalesced frame");
    require(snapshot.blocks.size() == 2 && snapshot.blocks[0].text == "hello" && snapshot.blocks[1].text == "world",
            "headless snapshots must expose the real transcript reducer state");
    require(!snapshot.ansi_operations.empty() && !snapshot.cells.empty(), "headless snapshots must expose ANSI operations and cells");
    require(session.inspect().layout == snapshot.layout, "inspection must not mutate layout diagnostics");

    tui::HeadlessSession command_session(80, 8);
    require(
        command_session.apply({.type = "tool_started", .call_id = "command", .name = "exec_command", .command = "echo ready"}).has_value(),
        "headless command start must apply");
    require(command_session.apply({.type = "advance_time", .milliseconds = 9'999}).has_value(),
            "headless command time must advance below the elapsed threshold");
    auto command_snapshot = command_session.inspect();
    std::string command_text;
    for (const auto &line : command_snapshot.visible_text) command_text += line + "\n";
    require(command_snapshot.blocks[0].command == "echo ready" && !command_text.contains("echo ready (9s)"),
            "headless snapshots must retain command data without early elapsed copy");
    require(command_session.apply({.type = "advance_time", .milliseconds = 1}).has_value(),
            "headless command time must cross the elapsed threshold");
    command_snapshot = command_session.inspect();
    command_text.clear();
    for (const auto &line : command_snapshot.visible_text) command_text += line + "\n";
    require(command_text.contains("• Running echo ready (10s)"),
            "virtual time must drive the same elapsed command refresh as the interactive timer");
}

void check_headless_repeated_activity_ids_across_turns() {
    tui::HeadlessSession session(48, 16);
    const auto ok = [&session](const tui::HeadlessAction &action) {
        require(session.apply(action).has_value(), "scoped headless lifecycle action must apply");
    };

    ok({.type = "submit", .text = "first"});
    ok({.type = "assistant_delta", .text = "first answer"});
    ok({.type = "assistant_message_completed", .text = "first answer"});
    ok({.type = "tool_started", .text = "first tool", .name = "read_file"});
    ok({.type = "tool_completed", .text = "first done", .name = "read_file"});
    ok({.type = "provider_activity_completed"});
    ok({.type = "task_completed"});

    ok({.type = "submit", .text = "second"});
    ok({.type = "assistant_delta", .text = "second answer"});
    ok({.type = "assistant_message_completed", .text = "second answer"});
    ok({.type = "tool_started", .text = "second tool", .name = "read_file"});
    ok({.type = "tool_completed", .text = "second done", .name = "read_file"});
    ok({.type = "provider_activity_completed"});
    ok({.type = "task_completed"});

    const auto snapshot = session.inspect();
    std::vector<const tui::SnapshotBlock *> assistant_blocks;
    std::vector<const tui::SnapshotBlock *> tool_blocks;
    const auto &blocks = snapshot.blocks;
    for (const auto &block : blocks) {
        if (block.kind == "assistant") assistant_blocks.push_back(&block);
        if (block.kind == "tool") tool_blocks.push_back(&block);
    }
    require(assistant_blocks.size() == 2 && assistant_blocks[0]->text == "first answer" && assistant_blocks[1]->text == "second answer",
            "two headless turns must retain assistant messages that reuse the default empty item ID");
    require(tool_blocks.size() == 2 && tool_blocks[0]->detail == "first done" && tool_blocks[1]->detail == "second done",
            "two headless turns must retain tool calls that reuse the default empty call ID");
    require(assistant_blocks[0]->task_generation != 0 && assistant_blocks[0]->task_generation != assistant_blocks[1]->task_generation &&
                assistant_blocks[0]->provider_call_generation != assistant_blocks[1]->provider_call_generation &&
                tool_blocks[0]->task_generation == assistant_blocks[0]->task_generation &&
                tool_blocks[0]->provider_call_generation == assistant_blocks[0]->provider_call_generation &&
                tool_blocks[1]->task_generation == assistant_blocks[1]->task_generation &&
                tool_blocks[1]->provider_call_generation == assistant_blocks[1]->provider_call_generation,
            "headless lifecycle actions must use nonzero task/provider scopes and advance them between turns");
}

void check_headless_selectable_list() {
    tui::HeadlessSession session(48, 9);
    require(session.apply({.type = "notice", .text = "underlying transcript"}).has_value(),
            "selectable-list fixture must seed the underlying screen");
    require(
        session.apply({.type = "open_list", .text = "one\ntwo", .name = "Choose item", .command = "Nothing here", .amount = 1}).has_value(),
        "headless selectable list must open");
    auto snapshot = session.inspect();
    require(snapshot.focused_surface == "selectable_list" && snapshot.selected_id == "one" &&
                snapshot.visible_text[0].contains("Choose item") && !snapshot.visible_text[2].contains("underlying transcript"),
            "selectable list must render as a focused layer without changing the underlying transcript");

    require(session.apply({.type = "down"}).has_value() && session.inspect().selected_id == "two",
            "Down must move selectable-list identity by one row");
    require(session.apply({.type = "down"}).has_value(), "Down at a page boundary must request another page");
    snapshot = session.inspect();
    require(snapshot.selection_effect == "load_next_page" && snapshot.visible_text[2].contains("Loading"),
            "page loading must be an observable deterministic list state");
    require(session.apply({.type = "list_next_page", .text = "three\nfour"}).has_value() && session.inspect().selected_id == "three",
            "page arrival must complete the outstanding Down navigation");
    require(session.apply({.type = "page_up"}).has_value() && session.inspect().selected_id == "one" &&
                session.apply({.type = "page_down"}).has_value() && session.inspect().selected_id == "three",
            "PageUp and PageDown must navigate loaded pages coherently");
    require(session.apply({.type = "submit"}).has_value() && session.inspect().selection_effect == "confirmed",
            "Enter must confirm the selected item identity");
    require(session.apply({.type = "close_list"}).has_value(), "headless selectable list must close");
    snapshot = session.inspect();
    std::string restored;
    for (const auto &line : snapshot.visible_text) restored += line;
    require(snapshot.focused_surface == "session" && restored.contains("underlying transcript"),
            "closing the list must reveal the unchanged underlying screen");

    require(session.apply({.type = "open_list", .name = "Empty", .command = "No sessions"}).has_value(), "empty selectable list must open");
    snapshot = session.inspect();
    require(snapshot.visible_text[2].contains("No sessions"), "empty selectable list must render its empty state");
    require(session.apply({.type = "submit"}).has_value() && session.inspect().selection_effect == "none" &&
                session.apply({.type = "escape"}).has_value() && session.inspect().selection_effect == "cancelled",
            "empty confirmation must be ignored and Esc must cancel deterministically");

    tui::HeadlessSession final_page(40, 8);
    require(final_page.apply({.type = "open_list", .text = "only", .amount = 1}).has_value() &&
                final_page.apply({.type = "page_down"}).has_value() && final_page.apply({.type = "list_next_page"}).has_value() &&
                final_page.inspect().selected_id == "only",
            "an empty final page must preserve the prior coherent selection");

    tui::HeadlessSession failed_page(40, 8);
    require(failed_page.apply({.type = "open_list", .text = "first", .amount = 1}).has_value() &&
                failed_page.apply({.type = "page_down"}).has_value() &&
                failed_page.apply({.type = "list_page_error", .text = "catalog failed"}).has_value(),
            "selectable-list page failure fixture must apply");
    snapshot = failed_page.inspect();
    require(snapshot.visible_text[2].contains("catalog failed") && snapshot.selected_id == "first",
            "page failure must render without losing the selected item identity");

    tui::SelectableList paged("Paged", "Empty",
                              {.items = {{.id = "one", .primary = "One"}, {.id = "two", .primary = "Two"}}, .has_more = true});
    require(paged.apply(tui::SelectableListAction::DOWN) == tui::SelectableListEffect::NONE &&
                paged.apply(tui::SelectableListAction::PAGE_DOWN) == tui::SelectableListEffect::LOAD_NEXT_PAGE,
            "PageDown boundary fixture must request its next page");
    paged.append_page({.items = {{.id = "three", .primary = "Three"}, {.id = "four", .primary = "Four"}}});
    require(paged.selected_id() == "four", "page arrival must complete PageDown at the prior row offset");

    tui::SelectableList backwards(
        "Backwards", "Empty", {.items = {{.id = "three", .primary = "Three"}, {.id = "four", .primary = "Four"}}, .has_previous = true});
    require(backwards.apply(tui::SelectableListAction::PAGE_UP) == tui::SelectableListEffect::LOAD_PREVIOUS_PAGE,
            "PageUp must request a bounded preceding page when replacement starts around a preferred identity");
    backwards.prepend_page({.items = {{.id = "one", .primary = "One"}, {.id = "two", .primary = "Two"}}});
    require(backwards.selected_id() == "one", "preceding page arrival must complete the outstanding PageUp navigation");
}

void check_headless_selectable_list_query() {
    tui::HeadlessSession loading(30, 8);
    require(loading.apply({.type = "open_loading_list", .name = "Loading search"}).has_value(),
            "initial searchable-list loading fixture must open");
    auto loading_snapshot = loading.inspect();
    require(loading_snapshot.picker_loading && !loading_snapshot.selected_id && loading_snapshot.visible_text[3].contains("Loading"),
            "initial loading must retain the focused empty query and expose no accidental selection");
    require(loading.apply({.type = "list_replace", .text = "first"}).has_value() && loading.inspect().selected_id == "first",
            "initial result replacement must select the first stable identity deterministically");

    tui::HeadlessSession session(18, 8);
    require(
        session.apply({.type = "open_list", .text = "one\ntwo", .name = "Searchable", .command = "No items", .amount = 1, .is_error = true})
            .has_value(),
        "searchable selectable-list fixture must open");
    require(session.apply({.type = "down"}).has_value() && session.inspect().selected_id == "two",
            "searchable list must establish a stable selected identity");
    std::string pasted_query = "ab\n\t中cd👩‍💻ef";
    pasted_query.push_back('\x7f');
    require(session.apply({.type = "insert", .text = pasted_query}).has_value(), "full-screen query paste must apply");
    auto snapshot = session.inspect();
    require(snapshot.selection_effect == "replace_results" && snapshot.picker_loading && snapshot.picker_query == "ab中cd👩‍💻ef" &&
                snapshot.selected_id == "two" && snapshot.visible_text[2].contains("Search:"),
            "query replacement must retain query and selection while loading");
    require(session.apply({.type = "list_replace", .text = "two"}).has_value(), "query result replacement must apply");
    require(session.apply({.type = "home"}).has_value() && session.apply({.type = "right"}).has_value() &&
                session.apply({.type = "right"}).has_value() && session.apply({.type = "right"}).has_value() &&
                session.apply({.type = "right"}).has_value(),
            "full-screen query grapheme movement must apply");
    snapshot = session.inspect();
    require(snapshot.picker_query_cursor == std::string("ab中c").size() && snapshot.cursor.visible,
            "full-screen query cursor must move by grapheme boundaries");
    require(session.apply({.type = "resize", .columns = 14, .rows = 7}).has_value(), "full-screen query resize must apply");
    snapshot = session.inspect();
    require(snapshot.picker_query == "ab中cd👩‍💻ef" && snapshot.picker_query_cursor == std::string("ab中c").size() &&
                snapshot.selected_id == "two" && snapshot.cursor.visible && snapshot.visible_text[2].contains("Search:"),
            "resize must preserve and cell-window the full-screen query, cursor, and identity");

    require(session.apply({.type = "end"}).has_value() && session.apply({.type = "backspace_word"}).has_value(),
            "query edit must start a replacement");
    require(session.apply({.type = "list_query_error", .text = "catalog unavailable"}).has_value(), "query failure transition must apply");
    snapshot = session.inspect();
    require(snapshot.picker_query == "ab中cd👩‍💻ef" && snapshot.selected_id == "two" &&
                snapshot.picker_error == "catalog unavailable" && snapshot.picker_visible_ids == std::vector<std::string>{"two"},
            "query failure must restore the prior usable query, result set, and selection");

    require(session.apply({.type = "close_list"}).has_value(), "failed query fixture must close");

    tui::HeadlessSession clearing(30, 8);
    require(
        clearing.apply(
                    {.type = "open_list", .text = "one\ntwo", .name = "Searchable", .command = "No items", .amount = 1, .is_error = true})
                .has_value() &&
            clearing.apply({.type = "down"}).has_value() && clearing.apply({.type = "insert", .text = "two"}).has_value() &&
            clearing.apply({.type = "list_replace", .text = "two"}).has_value() && clearing.apply({.type = "backspace_word"}).has_value() &&
            clearing.apply({.type = "list_replace", .text = "one\ntwo", .amount = 1}).has_value(),
        "clearing the query must restore the unfiltered result transition");
    snapshot = clearing.inspect();
    require(snapshot.picker_query.empty() && snapshot.selected_id == "two" &&
                snapshot.picker_visible_ids == std::vector<std::string>({"one", "two"}),
            "clearing a query must restore normal order while preserving a matching identity");
    require(clearing.apply({.type = "down"}).has_value() && clearing.apply({.type = "list_next_page", .text = "three"}).has_value() &&
                clearing.inspect().selected_id == "three",
            "appending an unfiltered page after clearing must complete boundary navigation");

    require(clearing.apply({.type = "insert", .text = "missing"}).has_value() && clearing.apply({.type = "list_replace"}).has_value(),
            "empty query result transition must apply");
    snapshot = clearing.inspect();
    require(!snapshot.selected_id && snapshot.visible_text[3].contains("No matches"),
            "empty filtered results must clear selection and show the query-specific empty state");
    require(clearing.apply({.type = "backspace_word"}).has_value() &&
                clearing.apply({.type = "list_replace", .text = "one\ntwo", .amount = 1}).has_value() &&
                clearing.inspect().selected_id == "one",
            "restoring results after selection disappearance must choose the first identity deterministically");
    require(clearing.apply({.type = "insert", .text = "x"}).has_value() && clearing.apply({.type = "escape"}).has_value() &&
                clearing.inspect().selection_effect == "cancelled",
            "Esc must cancel a full-screen picker immediately during a query transition");
}

void check_history_complete_text_filtering() {
    const std::vector<session::ConversationCheckpoint> checkpoints = {
        {.id = {.entry = {.value = 101}},
         .depth = 0,
         .label = "Deploy Alpha",
         .task_outcome = session::TaskOutcome::COMPLETED,
         .on_active_branch = true,
         .branch_leaf_count = 3,
         .branch_leaf_examples = {{.entry = {.value = 901}}, {.entry = {.value = 902}}}},
        {.id = {.entry = {.value = 202}},
         .depth = 1,
         .label = "Investigate failure",
         .task_outcome = session::TaskOutcome::FAILED,
         .active = true,
         .on_active_branch = true,
         .branch_leaf_count = 1,
         .branch_leaf_examples = {{.entry = {.value = 202}}}},
        {.id = {.entry = {.value = 303}},
         .depth = 1,
         .label = "Preserved Résumé",
         .task_outcome = session::TaskOutcome::CANCELLED,
         .branch_leaf_count = 1,
         .branch_leaf_examples = {{.entry = {.value = 303}}}},
    };
    const auto ids = [&](std::string_view query) {
        std::vector<std::string> result;
        for (const auto &item : tui::conversation_history_page(checkpoints, query).items) result.push_back(item.id);
        return result;
    };
    require(ids("deploy alpha") == std::vector<std::string>{"101"}, "history label matching failed");
    require(ids("202") == std::vector<std::string>{"202"}, "history full checkpoint ID matching failed");
    require(ids("failed") == std::vector<std::string>{"202"}, "history outcome matching failed");
    require(ids("current append point") == std::vector<std::string>{"202"}, "history active-state matching failed");
    require(ids("active ancestor") == std::vector<std::string>{"101"}, "history ancestor-state matching failed");
    require(ids("preserved branch") == std::vector<std::string>{"303"}, "history preserved-state matching failed");
    require(ids("#902") == std::vector<std::string>{"101"}, "history representative leaf-ID matching failed");
    require(ids("3 branches") == std::vector<std::string>{"101"}, "history branch-count description matching failed");
    require(ids("résumé") == std::vector<std::string>{"303"} && ids("RÉsumé").empty(),
            "history matching must fold ASCII while matching non-ASCII bytes exactly");
    require(ids("") == std::vector<std::string>({"101", "202", "303"}), "history filtering changed the original tree order");
}

void check_model_picker_headless_flow() {
    tui::HeadlessSession session(80, 24);
    auto ok = [&](const tui::HeadlessAction &action) { require(session.apply(action).has_value(), "headless action must apply"); };

    ok({.type = "insert", .text = "draft in progress"});
    ok({.type = "open_model_picker", .amount = 1});
    auto snapshot = session.inspect();
    require(snapshot.focused_surface == "compact_picker" && snapshot.picker_loading,
            "an opening picker must own focus and show its loading state");
    require(std::ranges::any_of(snapshot.visible_text, [](const std::string &row) { return row.contains("Loading…"); }),
            "the loading state must render a compact row");
    require(std::ranges::any_of(snapshot.visible_text, [](const std::string &row) { return row.starts_with("Model:"); }),
            "the owned query row must render while loading");

    ok({.type = "picker_items", .text = "alpha/one@low\nalpha/one@high\n*beta/one@high\ngamma/two"});
    ok({.type = "insert", .text = "one"});
    snapshot = session.inspect();
    require(!snapshot.picker_loading && snapshot.picker_query == "one" && snapshot.picker_visible_ids.size() == 3,
            "typing while the picker owns focus must edit the picker query");
    require(snapshot.composer_text == "draft in progress", "the composer draft must stay untouched behind the picker");

    ok({.type = "insert", .text = " HIGH"});
    require(session.inspect().picker_visible_ids.empty(), "a spaced query must reach the empty state");
    for (i32 step = 0; step < 5; ++step) ok({.type = "backspace"});
    ok({.type = "insert", .text = "high"});
    snapshot = session.inspect();
    require(snapshot.picker_query == "onehigh" && snapshot.picker_visible_ids.empty(),
            "backspace must edit the picker query, not the composer");
    for (i32 step = 0; step < 7; ++step) ok({.type = "backspace"});
    ok({.type = "insert", .text = "high"});
    snapshot = session.inspect();
    require(snapshot.picker_visible_ids == std::vector<std::string>{"alpha/one@high", "beta/one@high"},
            "case-insensitive substring matching must include effort labels");

    ok({.type = "down"});
    snapshot = session.inspect();
    require(snapshot.picker_highlight_id == "beta/one@high", "Down must move the picker highlight");

    ok({.type = "picker_error", .text = "Cannot refresh models: provider offline"});
    snapshot = session.inspect();
    require(snapshot.picker_error == "Cannot refresh models: provider offline" && snapshot.picker_query == "high" &&
                snapshot.picker_highlight_id == "beta/one@high",
            "an error must preserve the query and highlighted identity");
    require(std::ranges::any_of(snapshot.visible_text,
                                [](const std::string &row) { return row.contains("Cannot refresh models: provider offline"); }),
            "the error state must render a bounded compact row");

    ok({.type = "resize", .columns = 70, .rows = 20});
    snapshot = session.inspect();
    require(snapshot.picker_query == "high" && snapshot.picker_highlight_id == "beta/one@high",
            "resize must preserve the picker query and selection identity");

    ok({.type = "home"});
    require(session.inspect().picker_query_cursor == 0, "Home must move the query cursor to the start");
    ok({.type = "insert", .text = "not"});
    snapshot = session.inspect();
    require(snapshot.picker_query == "nothigh" && snapshot.picker_query_cursor == 3,
            "insertion must happen at the query cursor rather than the tail");
    require(snapshot.picker_visible_ids.empty(), "mid-query insertion must refilter");
    ok({.type = "delete_word"});
    snapshot = session.inspect();
    require(snapshot.picker_query == "not" && snapshot.picker_query_cursor == 3, "delete-word must erase the word after the query cursor");
    ok({.type = "backspace_word"});
    snapshot = session.inspect();
    require(snapshot.picker_query.empty() && snapshot.picker_query_cursor == 0,
            "backspace-word must erase the word before the query cursor");
    require(snapshot.picker_visible_ids.size() == 4, "clearing the query must restore the complete dataset");

    ok({.type = "insert", .text = "中"});
    ok({.type = "insert", .text = "x"});
    ok({.type = "left"});
    ok({.type = "left"});
    snapshot = session.inspect();
    require(snapshot.picker_query == "中x" && snapshot.picker_query_cursor == 0, "Left must move by grapheme boundaries");
    require(snapshot.cursor.visible && snapshot.cursor.column == tui::text_width("Model: "),
            "the frame cursor must track the query cursor");
    ok({.type = "right"});
    require(session.inspect().picker_query_cursor == 3, "Right must cross a multi-byte grapheme in one step");
    ok({.type = "end"});
    require(session.inspect().picker_query_cursor == 4, "End must move the query cursor to the end");
    ok({.type = "delete"});
    require(session.inspect().picker_query == "中x", "Delete at the end of the query must be a no-op");
    ok({.type = "word_left"});
    require(session.inspect().picker_query_cursor == 0, "word movement must operate on the query");
    ok({.type = "delete"});
    require(session.inspect().picker_query == "x", "Delete must erase the grapheme after the cursor");

    ok({.type = "escape"});
    snapshot = session.inspect();
    require(snapshot.focused_surface == "session" && snapshot.composer_text == "draft in progress",
            "cancelling the picker must return focus to the untouched composer");
}

void check_model_picker_rows_disambiguate_providers() {
    std::vector<model::Entry> entries;
    entries.push_back({.provider = "alpha", .id = "gpt-x", .name = "GPT X", .reasoning_efforts = {"low", "high"}});
    entries.push_back({.provider = "beta", .id = "gpt-x", .name = "gpt-x"});
    const auto items = tui::model_picker_items(entries, "alpha", "gpt-x", std::optional<std::string>("high"));

    std::vector<std::string> ids;
    for (const auto &item : items) ids.push_back(item.id);
    require(ids == std::vector<std::string>{"alpha/gpt-x@low", "alpha/gpt-x@high", "alpha/gpt-x@off", "beta/gpt-x"},
            "rows must expand declared efforts plus an explicit off variant and stay selector-addressable");
    require(items[0].primary == "alpha/gpt-x@low" && items[3].primary == "beta/gpt-x",
            "duplicate model IDs must stay distinguishable by provider in the row text");
    require(!items[0].current && items[1].current && !items[2].current && !items[3].current,
            "exactly the active provider, model, and effort row must carry the current marker");
    require(items[0].description == "GPT X" && items[3].description.empty(),
            "display names must appear only when they differ from the model ID");

    const auto effort_off = tui::model_picker_items(entries, "alpha", "gpt-x", std::nullopt);
    require(!effort_off[0].current && !effort_off[1].current && effort_off[2].current && !effort_off[3].current,
            "a null current effort must mark the @off row for a model with declared efforts");
    const auto other_current = tui::model_picker_items(entries, "beta", "gpt-x", std::nullopt);
    require(!other_current[0].current && !other_current[1].current && !other_current[2].current && other_current[3].current,
            "a model without declared efforts must mark its bare row when its effort is null");

    tui::CompactPicker picker;
    picker.set_items(items);
    picker.set_query("beta");
    require(picker.filtered.size() == 1 && picker.highlighted_id() == "beta/gpt-x", "provider text must be searchable");
    picker.set_query("gpt x");
    require(picker.filtered.size() == 3, "display names must be searchable");
    picker.set_query("off");
    require(picker.filtered.size() == 1 && picker.highlighted_id() == "alpha/gpt-x@off",
            "the off variant must be reachable through effort search");
}

void check_model_picker_shrinks_before_transcript_reserve() {
    tui::HeadlessSession tall(60, 24);
    auto ok_tall = [&](const tui::HeadlessAction &action) { require(tall.apply(action).has_value(), "headless action must apply"); };
    for (i32 index = 0; index < 6; ++index) ok_tall({.type = "notice", .text = "transcript row " + std::to_string(index)});
    ok_tall({.type = "open_model_picker", .text = "alpha/one\nbeta/two\ngamma/three\ndelta/four\nepsilon/five"});
    auto snapshot = tall.inspect();
    require(tall.screen.viewport_rows() >= 4, "a tall terminal must keep the guaranteed transcript reserve with the picker open");
    require(std::ranges::any_of(snapshot.visible_text, [](const std::string &row) { return row.starts_with("Model:"); }),
            "the tall band must render its query row");
    require(std::ranges::any_of(snapshot.visible_text, [](const std::string &row) { return row.contains("alpha/one"); }),
            "the tall band must render its result rows");
    require(std::ranges::any_of(snapshot.visible_text, [](const std::string &row) { return row.contains("transcript row"); }),
            "the transcript must stay visible above the band");
    require(snapshot.visible_text.back().contains("headless"), "the footer must stay on the last row");

    tui::HeadlessSession low(60, 8);
    auto ok_low = [&](const tui::HeadlessAction &action) { require(low.apply(action).has_value(), "headless action must apply"); };
    for (i32 index = 0; index < 6; ++index) ok_low({.type = "notice", .text = "transcript row " + std::to_string(index)});
    const auto viewport_before = low.screen.viewport_rows();
    ok_low({.type = "open_model_picker", .text = "alpha/one\nbeta/two"});
    snapshot = low.inspect();
    require(snapshot.focused_surface == "compact_picker", "the picker must stay active even when the band cannot render");
    require(std::ranges::none_of(snapshot.visible_text, [](const std::string &row) { return row.starts_with("Model:"); }),
            "the band must shrink to nothing before consuming the minimum transcript space");
    require(low.screen.viewport_rows() == viewport_before, "a band with no room must leave the transcript viewport untouched");
    require(snapshot.visible_text.back().contains("headless"), "the footer must survive on a short terminal");
}

void check_compact_picker_dialog_flow() {
    tui::ConsoleRenderer renderer;
    tui::CompactPickerDialog dialog;
    lighter::EventLoop loop;

    tui::CompactPicker picker{.query_label = "Model", .empty_message = "No matching model"};
    picker.loading = true;
    require(!dialog.begin(renderer, std::move(picker)), "compact picker dialog fixture failed to open");
    require(dialog.active() && renderer.model_picker_active(), "the dialog must expose the open picker");

    std::vector<tui::CompactPickerItem> items;
    for (auto id : {"alpha/one", "beta/two"}) {
        tui::CompactPickerItem item{.id = id, .primary = id};
        item.haystacks.push_back(id);
        items.push_back(std::move(item));
    }
    require(!dialog.set_items(items), "populating the dialog must succeed");
    require(!renderer.screen.picker->loading, "populating must clear the loading state");

    require(!dialog.confirm(), "confirm must succeed");
    require(!dialog.active(), "confirm must close the picker");
    auto decision = dialog.next();
    loop.schedule(decision);
    loop.run();
    auto selected = decision.result();
    require(selected.has_value() && *selected == "alpha/one", "confirm must record the highlighted identity");

    picker = tui::CompactPicker{.query_label = "Model", .empty_message = "No matching model"};
    require(!dialog.begin(renderer, std::move(picker)), "the dialog must reopen cleanly");
    require(!dialog.set_items(items), "repopulating must succeed");
    renderer.screen.picker->set_query("nothing-matches");
    require(!dialog.confirm() && dialog.active(), "Enter with no matching result must keep the picker open");
    require(!dialog.cancel() && !dialog.active(), "cancel must close the picker");
    auto cancelled = dialog.next();
    loop.schedule(cancelled);
    loop.run();
    require(!cancelled.result().has_value(), "cancel must record an empty decision");
}

void check_selectable_dialog_uses_generic_page_errors() {
    tui::ConsoleRenderer renderer;
    tui::SelectableListDialog dialog;
    tui::SelectableList list("Choose item", "No items",
                             tui::SelectableListPage{.items = {{.id = "first", .primary = "First"}}, .has_more = true});
    auto begun =
        dialog.begin(renderer, std::move(list),
                     [](std::string_view, tui::SelectableListPageLoad, std::optional<std::string_view>) -> Result<tui::SelectableListPage> {
                         return lighter::outcome_error(Error::storage("catalog failed"));
                     });
    require(!begun, "selectable dialog fixture failed to open");
    require(!dialog.apply(tui::SelectableListAction::PAGE_DOWN), "selectable dialog page failure did not render");
    require(renderer.selectable_list &&
                frame_text(renderer.selectable_list->frame(60, 8)).contains("Cannot load next page: catalog failed"),
            "reusable selectable dialog used domain-specific page error wording");
}

void check_selectable_dialog_passes_preferred_identity() {
    tui::ConsoleRenderer renderer;
    tui::SelectableListDialog dialog;
    tui::SelectableList list("Choose item", "No items",
                             tui::SelectableListPage{.items = {{.id = "one", .primary = "One"}, {.id = "two", .primary = "Two"}}});
    list.enable_query("No matches");
    std::optional<std::string> preferred;
    auto begun =
        dialog.begin(renderer, std::move(list),
                     [&preferred](std::string_view query, tui::SelectableListPageLoad load,
                                  std::optional<std::string_view> preferred_id) -> Result<tui::SelectableListPage> {
                         require(query == "broad" && load == tui::SelectableListPageLoad::REPLACE,
                                 "query replacement must identify its catalog transition");
                         if (preferred_id) preferred = *preferred_id;
                         return tui::SelectableListPage{.items = {{.id = "two", .primary = "Two"}, {.id = "three", .primary = "Three"}},
                                                        .has_previous = true,
                                                        .has_more = true};
                     });
    require(!begun && !dialog.apply(tui::SelectableListAction::DOWN), "preferred-identity dialog fixture failed to select its second row");
    require(!dialog.edit_query(tui::PickerQueryEdit::INSERT, "broad"), "preferred-identity replacement failed");
    require(preferred == "two" && renderer.selectable_list_selection() == "two",
            "replacement must pass and retain the selected stable identity even when the catalog opens away from its first page");
}

void check_headless_resize_and_markup_stress() {
    tui::HeadlessSession session(80, 24);
    u32 state = 0x243f6a88;
    auto next = [&state] {
        state = state * 1103515245u + 12345u;
        return state;
    };
    constexpr std::string_view fragments[] = {
        "plain text ", "**unfinished", "** done\n", "```cpp\n", "x();\n```\n", "\x1b[2J", "中", "👩‍💻", "\xff", "/", "/model x",
    };

    bool assistant_streaming = false;
    for (usize index = 0; index < 4000; ++index) {
        const auto choice = next() % 11;
        tui::HeadlessAction action;
        if (choice <= 2) {
            action.type = "assistant_delta";
            action.text = std::string(fragments[next() % std::size(fragments)]);
            assistant_streaming = true;
        } else if (choice == 3) {
            action.type = "resize";
            action.columns = 1 + static_cast<i32>(next() % 120);
            action.rows = 1 + static_cast<i32>(next() % 50);
        } else if (choice == 4) {
            action.type = "notice";
            action.text = "notice-" + std::to_string(index);
        } else if (choice == 5) {
            action.type = "scroll";
            action.amount = static_cast<i32>(next() % 41) - 20;
        } else if (choice == 6) {
            action.type = "advance_time";
            action.milliseconds = next() % 33;
        } else if (choice == 7) {
            action.type = "insert";
            action.text = std::string(fragments[next() % std::size(fragments)]);
        } else if (choice == 8) {
            action.type = next() % 2 == 0 ? "tab" : "escape";
        } else if (choice == 9) {
            action.type = next() % 4 == 0 ? "close_picker" : "open_model_picker";
            if (action.type == "open_model_picker") action.text = "alpha/one\nbeta/two\ngamma/three";
        } else if (assistant_streaming) {
            // The transcript reducer requires a streaming assistant block
            // before an assistant message can complete.
            action.type = "assistant_message_completed";
            assistant_streaming = false;
        } else {
            action.type = "assistant_delta";
            action.text = std::string(fragments[next() % std::size(fragments)]);
            assistant_streaming = true;
        }
        require(session.apply(action).has_value(), "bounded stress action must remain valid");

        if (index % 97 == 0) {
            const auto snapshot = session.inspect();
            require(snapshot.columns >= 1 && snapshot.columns <= 500 && snapshot.rows >= 1 && snapshot.rows <= 200,
                    "stress projection must preserve terminal bounds");
            require(snapshot.visible_text.size() == static_cast<usize>(snapshot.rows),
                    "stress projection must expose exactly one string per terminal row");
            if (snapshot.cursor.visible) {
                require(snapshot.cursor.row >= 0 && snapshot.cursor.row < snapshot.rows && snapshot.cursor.column >= 0 &&
                            snapshot.cursor.column < snapshot.columns,
                        "stress projection must keep a visible cursor inside the terminal");
            }
            for (const auto &cell : snapshot.cells) {
                require(cell.row >= 0 && cell.row < snapshot.rows && cell.column >= 0 && cell.column < snapshot.columns,
                        "stress projection emitted an out-of-bounds cell");
                require(lighter::encoding::utf8::is_valid(cell.text), "stress projection emitted invalid UTF-8");
            }
        }
    }

    const auto snapshot = session.inspect();
    require(snapshot.action_count == 4000, "stress session must apply every generated action");
    require(snapshot.text_bytes <= 8 * 1024 * 1024, "stress session must remain inside its declared text budget");
}

i32 run_all(std::string_view executable) {
    check_surface_cells_and_encoding();
    check_header_identity_and_path_presentation();
    check_composer_editing();
    check_multiline_navigation_history_and_projection();
    check_responsive_footer();
    check_rich_output_and_concurrent_tools();
    check_external_editor_round_trip(executable);
    check_clipboard_helper();
    check_command_parsing_and_status();
    check_help_notice_lists_registry();
    check_compact_picker_filtering_and_states();
    check_command_menu_activation_boundary();
    check_command_menu_key_semantics();
    check_command_menu_rows_and_availability();
    check_empty_session_hint();
    check_scroll_resize_state();
    check_repeated_activity_ids_are_provider_scoped();
    check_activity_state();
    check_mouse_selection();
    check_headless_virtual_time_and_snapshots();
    check_headless_repeated_activity_ids_across_turns();
    check_headless_selectable_list();
    check_headless_selectable_list_query();
    check_history_complete_text_filtering();
    check_model_picker_headless_flow();
    check_model_picker_rows_disambiguate_providers();
    check_model_picker_shrinks_before_transcript_reserve();
    check_compact_picker_dialog_flow();
    check_selectable_dialog_uses_generic_page_errors();
    check_selectable_dialog_passes_preferred_identity();
    check_headless_resize_and_markup_stress();
    return 0;
}

} // namespace

i32 main(i32 argc, char **argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--external-editor-helper") {
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output << "edited externally\n\n";
        return output ? 0 : 1;
    }
    try {
        return run_all(argv[0]);
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
