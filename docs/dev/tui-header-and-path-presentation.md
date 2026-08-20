# TUI Header and Path Presentation

The interactive TUI stores header identity as semantic application state and projects it to terminal cells only while constructing a frame.
This keeps session identity stable across resize and lets the normal session screen and contextual full-screen pickers share one presentation
contract.

## Semantic state

`SessionHeader` contains four width-independent values:

- the invocation workspace path used by tools;
- an optional user home directory;
- the optional explicit session title;
- the first-user-prompt preview, bounded by the session domain's canonical 240-byte valid-UTF-8 policy.

`SessionScreen` owns this state. `/name`, `/name --clear`, the first submitted prompt, resume switching, and fork switching update it directly.
Transcript loading and width changes do not derive or replace identity. The noninteractive renderer retains its existing append-only output.

Workspace conversion and home discovery occur once during REPL setup, outside `SessionScreen::frame()`, and cross the TUI boundary as valid
UTF-8 strings. On Windows, the platform implementation converts the native wide workspace path and reads home variables through the wide
environment API. POSIX native bytes are UTF-8-sanitized. Frame construction performs no environment, filesystem, catalog, or database work.

## Pure projection

`header_presentation.*` provides pure functions for title normalization, title resolution, workspace projection, and complete header composition.
Both `SessionScreen` and contextual `SelectableList` instances call this projector.

Title resolution is:

1. normalized explicit title;
2. normalized first-prompt preview;
3. `New session`.

Normalization produces one terminal-safe line by replacing unsafe control input, collapsing layout whitespace, and trimming leading/trailing
whitespace. Final bounding and truncation operate on extended grapheme clusters and terminal-cell widths supplied by the existing surface
utilities.

## Foreign-style path parsing

The projector recognizes POSIX roots, Windows drive roots, Windows UNC roots, `/`, and `\` separators directly from strings. It does not use
host `std::filesystem::path` parsing, so Windows paths have the same semantics on Linux and POSIX paths have the same semantics on Windows. A
leading POSIX root fixes `/` as the only separator, preserving a literal backslash inside a legal POSIX component.

Home containment compares complete parsed components. POSIX components are case-sensitive; Windows drive and UNC components use ASCII
case-insensitive comparison. A similar byte prefix is not containment. When containment succeeds, the home prefix becomes `~` while the
workspace's separator convention is retained.

Path projection uses these representations in order:

1. the full path, after optional home substitution;
2. a fish-like path with each intermediate component reduced to its first grapheme;
3. a cell-bounded path that retains the root and complete final component whenever the budget permits.

Hidden intermediate components retain `.` plus the first grapheme after it. Combining sequences, wide characters, flags, emoji, and ZWJ
sequences are never split. An impossible or zero cell budget degrades deterministically without writing beyond the row.

## Header allocation and surfaces

The normal row is `liminal · <workspace> · <title>`. Once product identity and separators are accounted for, workspace presentation receives
the maximum paired-content budget while preserving a minimal title signal; the resolved title uses the remaining cells. This makes workspace
projection follow full, fish-like, then truncated priority before progressively degrading the title. Extremely narrow rows clip product
identity safely.

The main full-screen surfaces use the same path and title rules:

- `/resume`: `Resume Session · <workspace>`;
- `/history`: `Conversation History · <workspace> · <title>`.

Checkpoint action and unsaved-history confirmation dialogs keep their own concise titles and do not opt into contextual headers.

The header remains exactly one row under the existing terminal-height rules. Composer sizing, compact-picker placement, transcript viewport
height, tail-following, and semantic viewport anchors are independent of header content. The footer no longer owns or renders workspace data;
its current fields are model/effort, remaining context, token usage, and persistence/status overrides.

## Verification contract

Tests exercise path and title projection as pure functions and inspect production frames through the headless TUI adapter. Coverage includes
foreign path styles, home boundaries, hidden and Unicode components, full/fish/truncated forms, contextual picker headers, state refresh, and
resize in both tail-following and transcript-browsing states. Visible assertions use surface rows and cells rather than relying only on encoded
ANSI output.
