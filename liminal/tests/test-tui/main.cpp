#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/encoding/utf8.h>

#include <liminal/event.h>
#include <liminal/tui/clipboard.h>
#include <liminal/tui/headless.h>
#include <liminal/tui/external_editor.h>
#include <liminal/tui/rich_text.h>
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
    require(encoded_palette.contains("\x1b[1;38;2;203;166;247mK"), "code keywords must use bold, high-luminance mauve");
    require(encoded_palette.contains("\x1b[1;38;2;250;179;135mK"), "code preprocessors must use distinct bold, high-luminance peach");
    require(encoded_palette.contains("\x1b[22;38;2;205;214;244mK"), "generic block code must use a calm near-foreground tint");
    require(encoded_palette.contains("\x1b[22;38;2;249;226;175mK"), "inline code must keep a warm accent distinct from prose");
    require(!encoded_palette.contains("\x1b[2m") && !encoded_palette.contains("\x1b[90m"),
            "syntax highlighting must not dim text or use a dark comment color");

    tui::Frame diff_palette{.surface = tui::Surface(3, 1)};
    diff_palette.surface.write(0, 0, "+", tui::Style::DIFF_ADDITION);
    diff_palette.surface.write(0, 1, "-", tui::Style::DIFF_DELETION);
    diff_palette.surface.write(0, 2, "@", tui::Style::DIFF_HUNK);
    const auto encoded_diff = tui::encode_frame(diff_palette);
    require(encoded_diff.contains("\x1b[22;38;2;166;227;161m+") && encoded_diff.contains("\x1b[22;38;2;243;139;168m-") &&
                encoded_diff.contains("\x1b[1;38;2;116;199;236m@"),
            "diff styles must use fixed truecolor so brightness never depends on terminal ANSI definitions");

    tui::Frame footer_palette{.surface = tui::Surface(4, 1)};
    constexpr tui::Style footer_styles[] = {
        tui::Style::FOOTER_MODEL,
        tui::Style::FOOTER_WORKSPACE,
        tui::Style::FOOTER_CONTEXT,
        tui::Style::FOOTER_TOKENS,
    };
    for (usize index = 0; index < std::size(footer_styles); ++index) {
        footer_palette.surface.write(0, static_cast<i32>(index), "F", footer_styles[index]);
    }
    const auto encoded_footer = tui::encode_frame(footer_palette);
    require(encoded_footer.contains("\x1b[22;38;2;249;226;175mF") && encoded_footer.contains("\x1b[22;38;2;166;227;161mF") &&
                encoded_footer.contains("\x1b[22;38;2;250;179;135mF") && encoded_footer.contains("\x1b[22;38;2;137;180;250mF"),
            "footer metadata fields must encode four distinct high-luminance colors");

    tui::Frame composer{.surface = tui::Surface(4, 1)};
    composer.surface.fill_row(0, tui::Style::COMPOSER);
    composer.surface.write(0, 0, ">", tui::Style::COMPOSER);
    require(std::ranges::all_of(composer.surface.cells, [](const tui::Cell &cell) { return cell.style == tui::Style::COMPOSER; }),
            "a filled composer row must retain its background style across every cell");
    require(tui::encode_frame(composer).contains("\x1b[22;39;48;2;38;38;38m>   "),
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
    visual.set_footer({.workspace_path = "D:\\code\\liminal", .context_left_percent = 54, .tokens_used = 1'184'000});
    const auto empty = visual.frame();
    const auto row_uses_style = [&empty](i32 row, tui::Style style) {
        const auto begin = empty.surface.cells.begin() + static_cast<isize>(row * empty.surface.columns);
        return std::ranges::all_of(begin, begin + empty.surface.columns, [style](const tui::Cell &cell) { return cell.style == style; });
    };
    require(empty.surface.row_text(0) == "liminal", "the header must keep product identity separate from input metadata");
    require(empty.surface.row_text(6).empty() && empty.surface.row_text(7) == "›" && empty.surface.row_text(8).empty() &&
                row_uses_style(6, tui::Style::COMPOSER) && row_uses_style(7, tui::Style::COMPOSER) &&
                row_uses_style(8, tui::Style::COMPOSER),
            "an empty composer must render as a padded three-row input surface");
    require(empty.cursor.row == 7 && empty.cursor.column == 2, "the composer cursor must begin after its concise prompt marker");
    require(empty.surface.row_text(9) == "test-model high · D:\\code\\liminal · Cont" &&
                empty.surface.cells[static_cast<usize>(9 * empty.surface.columns)].style == tui::Style::FOOTER_MODEL,
            "the footer must place model, workspace, context, and usage metadata below the composer");

    visual.resize({80, 10});
    const auto wide_footer = visual.frame();
    const auto footer_row = static_cast<usize>(9 * wide_footer.surface.columns);
    const auto footer_style = [&wide_footer, footer_row](std::string_view text, tui::Style style) {
        const auto row = wide_footer.surface.row_text(9);
        const auto byte_offset = row.find(text);
        if (byte_offset == std::string::npos) return false;
        return wide_footer.surface.cells[footer_row + static_cast<usize>(tui::text_width(row.substr(0, byte_offset)))].style == style;
    };
    require(wide_footer.surface.row_text(9) == "test-model high · D:\\code\\liminal · Context 54% left · 1.18M used",
            "the footer must format all four session metadata items compactly");
    require(footer_style("test-model high", tui::Style::FOOTER_MODEL) && footer_style("D:\\code\\liminal", tui::Style::FOOTER_WORKSPACE) &&
                footer_style("Context 54% left", tui::Style::FOOTER_CONTEXT) && footer_style("1.18M used", tui::Style::FOOTER_TOKENS),
            "each footer metadata item must use its own semantic color");

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
    require(active.state == tui::SessionState::STREAMING, "editing a draft during a turn must not overwrite the turn's semantic state");

    active.external_editor_active = true;
    active.resize({40, 8});
    require(frame_text(active.frame()).contains("Save and close external editor"),
            "the status row must explain how to return from an external editor handoff");
}

void check_rich_output_and_concurrent_tools() {
    constexpr std::string_view fixture = R"md(# Rich output
Paragraph with **strong**, *emphasis*, `inline code`, and [docs](https://example.com/docs).
The foo_bar_baz identifier stays literal.
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
    require(wide_text.contains("assistant: Rich output") && !wide_text.contains("# Rich output"),
            "headings must render without their Markdown marker");
    require(wide_text.contains("strong") && !wide_text.contains("**strong**") && has_style(wide, tui::Style::EMPHASIS) &&
                has_style(wide, tui::Style::ITALIC),
            "strong and emphasis markup must become terminal styles");
    require(wide_text.contains("foo_bar_baz"), "intraword underscores in code-like identifiers must remain literal");
    require(wide_text.contains("docs") && wide_text.contains("<https://example.com/docs>") && has_style(wide, tui::Style::LINK),
            "links must retain a visible, styled target");
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
    require(nested_text.contains("assistant: ┌ markdown") && nested_text.contains("│ ```nested```") && nested_text.ends_with("└"),
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
    streaming.apply(AssistantSegmentCompleted{});
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
    require(tools.state == tui::SessionState::STREAMING, "tool state may settle only after the final concurrent tool completes");
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

void check_copy_command_and_status() {
    const auto newest = tui::parse_copy_command("/copy");
    const auto older = tui::parse_copy_command("/copy  3\t");
    const auto unrelated = tui::parse_copy_command("/copycat");
    const auto zero = tui::parse_copy_command("/copy 0");
    const auto trailing = tui::parse_copy_command("/copy 2 more");
    require(newest.has_value() && *newest == std::optional<usize>{1}, "/copy must select the newest reply");
    require(older.has_value() && *older == std::optional<usize>{3}, "/copy N must use a newest-first positive ordinal");
    require(unrelated.has_value() && !unrelated->has_value(),
            "slash commands that merely share the copy prefix must remain ordinary prompts");
    require(!zero && !trailing && zero.error().detail.contains("usage"), "malformed copy ordinals must report the command usage");

    tui::SessionScreen screen;
    screen.resize({80, 10});
    screen.set_model("test-model", std::nullopt);
    screen.show_status("Copied latest reply to clipboard");
    const auto copied = screen.frame();
    require(frame_text(copied).contains("Copied latest reply to clipboard") &&
                copied.surface.cells[static_cast<usize>(9 * copied.surface.columns)].style == tui::Style::ACCENT,
            "copy success must temporarily replace the footer with an emphasized textual confirmation");

    screen.insert("next prompt");
    const auto editing = frame_text(screen.frame());
    require(!editing.contains("Copied latest reply") && editing.contains("test-model"),
            "the copy confirmation must yield to normal footer metadata on the next interaction");

    screen.show_status("Copied latest reply to clipboard");
    screen.external_editor_active = true;
    const auto editor = screen.frame();
    require(frame_text(editor).contains("Save and close external editor") &&
                editor.surface.cells[static_cast<usize>(9 * editor.surface.columns)].style == tui::Style::MUTED,
            "external-editor guidance must take priority over a copy confirmation");
}

void check_scroll_resize_and_unread_state() {
    tui::SessionScreen screen;
    screen.resize({18, 8});
    screen.set_model("test-model", std::optional<std::string>("high"));
    for (i32 index = 0; index < 9; ++index) {
        screen.apply(SessionNotice{.text = "line-" + std::to_string(index)});
    }

    const auto tail = screen.frame();
    require(frame_text(tail).contains("line-8"), "following viewport must show transcript tail");
    require(tail.surface.row_text(0) == "liminal", "the header must retain concise product identity");

    screen.move_up();
    require(!screen.anchor.has_value(), "Up at a composer boundary must not scroll the transcript");
    screen.move_down();
    require(!screen.anchor.has_value(), "Down at a composer boundary must not scroll the transcript");

    screen.page(-1);
    require(screen.anchor.has_value(), "PageUp must establish a semantic viewport anchor");
    const auto original_anchor = *screen.anchor;
    const auto history = screen.frame();
    require(frame_text(history).contains("history"), "browsing state must be visible");
    require(!frame_text(history).contains("line-8"), "PageUp must move away from the transcript tail");

    screen.apply(SessionNotice{.text = "line-9"});
    require(screen.anchor == original_anchor, "new output must preserve a browsing anchor");
    require(screen.unread, "new output while browsing must set the unread indicator");

    screen.resize({10, 8});
    require(screen.anchor == original_anchor, "resize must preserve the semantic source anchor");
    const auto resized = screen.frame();
    require(resized.surface.row_text(7).starts_with("history"), "resize must deterministically reflow browsing status");
    require(tui::encode_frame(resized) == tui::encode_frame(screen.frame()), "unchanged state must produce an identical frame");
    const auto diagnostics = screen.layout_diagnostics();
    require(diagnostics.cache_hits > 0 && diagnostics.cached_blocks > 0, "repeated frames must reuse stable block layout");

    screen.follow_tail();
    require(!screen.anchor && !screen.unread, "returning to tail must clear browsing and unread state");
    require(frame_text(screen.frame()).contains("line-9"), "returning to tail must reveal the newest output");
}

void check_working_indicator() {
    auto now = std::chrono::steady_clock::time_point{};
    tui::SessionScreen screen([&now] { return now; });
    screen.resize({40, 10});
    screen.set_model("test-model", std::nullopt);
    const auto idle_rows = screen.viewport_rows();
    require(!screen.working() && !frame_text(screen.frame()).contains("Thinking"), "an idle session must not show the working indicator");

    screen.apply(PromptSubmitted{.text = "go"});
    require(screen.working() && screen.animating(), "a submitted prompt must enter the animated working state");
    require(screen.viewport_rows() == idle_rows, "the in-flow working status must not shrink the transcript viewport");
    const auto waiting = screen.frame();
    require(waiting.surface.row_text(1) == "you: go" && waiting.surface.row_text(2).empty() &&
                waiting.surface.row_text(3).contains("Thinking…"),
            "a waiting turn must show the status one blank line below the newest output");
    const auto dot_style = [&screen] {
        const auto frame = screen.frame();
        return frame.surface.cells[static_cast<usize>(3 * frame.surface.columns)].style;
    };
    require(dot_style() == tui::Style::MUTED, "the working dot must start at its dim pulse phase");
    now += std::chrono::milliseconds(200);
    require(dot_style() == tui::Style::NORMAL, "the working dot must brighten as the pulse advances");
    now += std::chrono::milliseconds(200);
    require(dot_style() == tui::Style::EMPHASIS && screen.frame().surface.row_text(3).contains("Thinking…"),
            "the pulse must peak at bold intensity without disturbing the state label");
    now += std::chrono::seconds(3);
    require(frame_text(screen.frame()).contains("(3s)"), "long waits must add a muted elapsed suffix");

    screen.apply(AssistantTextDelta{.text = "hi"});
    require(frame_text(screen.frame()).contains("Writing…"), "streaming turns must show the writing status");
    screen.apply(ToolStarted{.call_id = "tool", .name = "exec_command", .command = "echo hi"});
    require(frame_text(screen.frame()).contains("Running tools…"), "running tools must show the tool status");
    screen.apply(ToolCompleted{.call_id = "tool", .name = "exec_command", .command = "echo hi", .summary = "exit 0"});
    require(frame_text(screen.frame()).contains("Writing…"), "the status must settle back once every tool completes");

    for (i32 index = 0; index < 12; ++index) screen.apply(SessionNotice{.text = "line-" + std::to_string(index)});
    require(frame_text(screen.frame()).contains("Writing…"), "a full viewport must keep the status at the transcript tail");
    screen.page(-1);
    require(screen.anchor.has_value() && !frame_text(screen.frame()).contains("Writing…"),
            "browsing history must scroll the working status away with the flow");
    screen.follow_tail();
    require(frame_text(screen.frame()).contains("Writing…"), "returning to the tail must reveal the working status again");

    screen.apply(TurnCompleted{});
    require(!screen.working() && !screen.animating(), "a completed turn must leave the animated working state");
    require(!frame_text(screen.frame()).contains("Writing…"), "a completed turn must remove the working status");
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

void check_headless_resize_and_markup_stress() {
    tui::HeadlessSession session(80, 24);
    u32 state = 0x243f6a88;
    auto next = [&state] {
        state = state * 1103515245u + 12345u;
        return state;
    };
    constexpr std::string_view fragments[] = {
        "plain text ", "**unfinished", "** done\n", "```cpp\n", "x();\n```\n", "\x1b[2J", "中", "👩‍💻", "\xff",
    };

    for (usize index = 0; index < 4000; ++index) {
        const auto choice = next() % 8;
        tui::HeadlessAction action;
        if (choice <= 2) {
            action.type = "assistant_delta";
            action.text = std::string(fragments[next() % std::size(fragments)]);
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
        } else {
            action.type = "assistant_segment_completed";
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
    check_composer_editing();
    check_multiline_navigation_history_and_projection();
    check_rich_output_and_concurrent_tools();
    check_external_editor_round_trip(executable);
    check_clipboard_helper();
    check_copy_command_and_status();
    check_scroll_resize_and_unread_state();
    check_working_indicator();
    check_mouse_selection();
    check_headless_virtual_time_and_snapshots();
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
