#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/types.hpp>
#include <lighter/encoding/utf8.h>

#include <liminal/event.h>
#include <liminal/tui/headless.h>
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

    tui::Frame palette{.surface = tui::Surface(1, 1)};
    palette.surface.write(0, 0, "K", tui::Style::CODE_KEYWORD);
    require(tui::encode_frame(palette).contains("\x1b[95mK"), "code keywords must use bright ANSI magenta");

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
    screen.insert("second");
    require(screen.take_prompt() == "second", "later submissions must remain independent");
    screen.insert("scratch");
    screen.previous_prompt();
    require(screen.composer.text == "second", "explicit previous-prompt navigation must recall the newest prompt");
    screen.previous_prompt();
    require(screen.composer.text == "first", "repeated previous-prompt navigation must walk older history");
    screen.next_prompt();
    screen.next_prompt();
    require(screen.composer.text == "scratch", "next-prompt navigation past the newest entry must restore the original draft");

    screen.clear_prompt();
    screen.insert("one\ntwo\nthree\nfour");
    const auto frame = screen.frame();
    require(screen.viewport_rows() == 4, "a growing composer must retain transcript viewport space");
    require(frame.surface.row_text(6) == "two" && frame.surface.row_text(7) == "three" && frame.surface.row_text(8) == "four",
            "an overflowing composer must vertically window around its cursor");
    require(frame.cursor.row == 8 && frame.cursor.column == 4, "the multiline composer cursor must remain visible on its logical row");

    tui::SessionScreen active;
    active.apply(AssistantTextDelta{.text = "streaming"});
    active.insert("queued draft");
    require(active.state == tui::SessionState::STREAMING, "editing a draft during a turn must not overwrite the turn's semantic state");
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
    require(text_has_style(unknown_language, "if value == 7", tui::Style::CODE),
            "unknown fenced languages must fall back to the generic code style");
    const auto python = tui::layout_rich_text("```Python3\ndef greet(name=\"Ada\"): # welcome\n    return 1\n```", 50);
    require(text_has_style(python, "def", tui::Style::CODE_KEYWORD) && text_has_style(python, "\"Ada\"", tui::Style::CODE_STRING) &&
                text_has_style(python, "# welcome", tui::Style::CODE_COMMENT) && text_has_style(python, "1", tui::Style::CODE_NUMBER),
            "language aliases must select the matching lightweight lexer family");
    const auto json = tui::layout_rich_text("```json\n{\"name\": \"liminal\", \"ready\": true}\n```", 50);
    require(text_has_style(json, "{\"name\": \"liminal\", \"ready\": true}", tui::Style::CODE),
            "unsupported fenced languages must remain plain code");

    tui::CodeHighlighter guarded("cpp");
    const auto oversized = guarded.highlight_line(std::string(4097, 'x'));
    require(!guarded.supported() && oversized.size() == 1 && oversized.front().style == tui::Style::CODE,
            "oversized source lines must disable highlighting and retain generic code output");

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

    tui::SessionScreen tools;
    tools.apply(ToolStarted{.call_id = "one", .name = "read_file"});
    tools.apply(ToolStarted{.call_id = "two", .name = "run_tests"});
    tools.apply(ToolCompleted{.call_id = "one", .name = "read_file"});
    require(tools.state == tui::SessionState::RUNNING_TOOLS, "one completed tool must not hide a concurrently running sibling");
    tools.apply(ToolCompleted{.call_id = "two", .name = "run_tests"});
    require(tools.state == tui::SessionState::STREAMING, "tool state may settle only after the final concurrent tool completes");
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
    require(tail.surface.row_text(0).starts_with("liminal  test"), "header must retain model identity when clipped");

    screen.move_up();
    require(screen.anchor.has_value(), "Up at a composer boundary must scroll the transcript by one row");
    screen.move_down();
    require(!screen.anchor.has_value(), "Down at a composer boundary must return a one-row scroll to the transcript tail");

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
    require(resized.surface.row_text(6).starts_with("history"), "resize must deterministically reflow browsing status");
    require(tui::encode_frame(resized) == tui::encode_frame(screen.frame()), "unchanged state must produce an identical frame");
    const auto diagnostics = screen.layout_diagnostics();
    require(diagnostics.cache_hits > 0 && diagnostics.cached_blocks > 0, "repeated frames must reuse stable block layout");

    screen.follow_tail();
    require(!screen.anchor && !screen.unread, "returning to tail must clear browsing and unread state");
    require(frame_text(screen.frame()).contains("line-9"), "returning to tail must reveal the newest output");
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

i32 run_all() {
    check_surface_cells_and_encoding();
    check_composer_editing();
    check_multiline_navigation_history_and_projection();
    check_rich_output_and_concurrent_tools();
    check_scroll_resize_and_unread_state();
    check_headless_virtual_time_and_snapshots();
    check_headless_resize_and_markup_stress();
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
