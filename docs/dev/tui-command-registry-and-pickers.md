# TUI Command Registry and Pickers

Architecture notes for the slash-command registry, compact command and model pickers, and filtered full-screen selection lists. User-visible behavior is described in `docs/user/tui.md`.

## Command registry

`liminal/tui/command.h` holds one data-driven registry: a `constexpr` array of `CommandSpec` entries with the canonical name, aliases, argument synopsis, one-line description, and idle-only availability of every executable slash command. It is the single authority for command names and presentation metadata:

- `resolve_command` and `find_command` resolve canonical names and aliases from it.
- The compact command menu builds its rows from it.
- `/help` (`describe_commands`) renders it as a durable transcript notice.
- Tests assert a bijection between `CommandKind` and registry entries and that removed commands stay absent.

Execution stays with the dispatcher switch in `repl_body`; `idle_only` is presentation metadata only, and the dispatcher's own idle guards remain the executable truth. Adding a command means adding one registry entry, one `CommandKind`, and one dispatch case — parsing, completion, and `/help` follow automatically.

## Compact picker primitive

`CompactPicker` (`liminal/tui/compact_picker.h`) is the bounded contextual selection surface rendered as a band directly above the composer. It owns query filtering (PREFIX or SUBSTRING over pre-lowercased haystacks), a highlight preserved by item identity across query and item changes, and explicit `loading` / `error` / empty states. The owned query carries a grapheme-aware byte cursor edited through `edit_query` (insert, backspace/delete, word variants, Left/Right/Word/Home/End). `project` renders into a caller-chosen row range; the owned query line renders nearest the composer and returns the frame cursor at the query cursor position.

Matching contract: filtering is case-insensitive for ASCII only — haystacks and queries are folded with byte-wise ASCII lowering. Provider IDs, model IDs, effort labels, and command names are ASCII by construction; non-ASCII text (display names, arbitrary queries) matches exactly, byte for byte. This is deliberate: the vendored libunicode provides segmentation and width but no case folding, and full Unicode folding is not worth a dependency for identifier search. Revisit if catalogs ever carry cased non-ASCII display names in practice.

Band placement lives in `SessionScreen::frame`: the band takes rows between the transcript viewport and the composer, and `viewport_rows()` subtracts it. Its budget is `base_viewport − k_transcript_reserve (4)`, capped at 8 result rows plus chrome, so it shrinks to zero before consuming the guaranteed minimum transcript space. A band with zero rows is invisible but its key handling stays active. All picker state is width-independent, so resize preserves query and selection identity for free.

## Command menu derivation

The command menu (`CommandMenu` inside `SessionScreen`) is composer-driven: its query is the command-name token itself and it renders no query row. Openness is **derived**, not evented:

```
open iff draft starts with '/' and not "//"
     and 1 <= cursor <= end of the name token (first whitespace, including '\n')
     and not dismissed
```

`sync_command_menu()` runs at the end of every composer-mutating `SessionScreen` wrapper (via `mark_editing` and `take_prompt`), which covers typing, paste, prompt-history recall, and external-editor replacement. Any composer mutation path that bypasses those wrappers would desync the menu — keep `Composer` mutations behind the `SessionScreen` methods.

Dismissal policy: Esc records the current token as dismissed. The dismissal clears when the token changes (editing reopens the menu) or stops existing (draft cleared, submitted, or no longer command-shaped). Moving the cursor out of and back into an unchanged dismissed token does not reopen the menu.

## Key semantics

`SessionScreen::apply_picker_key(PickerKey) -> {HANDLED, SUBMIT, PASS}` decides compact-surface key handling once, shared by the live REPL (`apply_terminal_event`) and the headless adapter. `PASS` means no compact surface consumed the key; `SUBMIT` asks the caller to run its normal Enter path.

| Key                                              | Command menu                                                                                                                 | Model picker                                          |
| ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| printable / paste                                | composer insert; derivation refilters                                                                                        | inserts at the query cursor; draft untouched          |
| Backspace / Delete (+Ctrl for word)              | composer; refilters or closes                                                                                                | edits the query at its cursor                         |
| Left / Right / Home / End (+Ctrl for word moves) | composer cursor (may close/reopen the menu)                                                                                  | moves the query cursor                                |
| Up / Down                                        | move highlight (clamped)                                                                                                     | move highlight                                        |
| Tab                                              | complete highlighted canonical name; menu stays open                                                                         | no-op                                                 |
| Enter                                            | exact name or alias → SUBMIT (executes); else complete without executing; empty result set → SUBMIT (command error boundary) | confirm highlighted row (no-op on empty results)      |
| Esc                                              | dismiss menu, draft untouched                                                                                                | cancel picker; also cancels an in-flight refresh task |

Esc priority: a compact surface closes first; a second Esc cancels the active task. The model picker waiting on a catalog refresh is the one case where a single Esc does both, because the picker is that pending task's only surface.

The full-screen `SelectableListDialog` branch stays first in `apply_terminal_event`; a full-screen dialog and a compact surface are never active at the same time.

## Shared picker query editing

`picker_query.*` is the single query-editing primitive used by compact and full-screen pickers. It applies insertion, paste, grapheme deletion and movement, word deletion and movement, Home, and End to a UTF-8 byte cursor. Insertions discard C0 controls and Delete before changing the query, so terminal paste cannot create an invisible newline or tab. Its cell-aware query window keeps that cursor visible without splitting an extended grapheme cluster. Query text and cursor position are model state and therefore survive resize; frame projection only reads them.

Picker matching folds ASCII `A`–`Z` only. Non-ASCII bytes are compared exactly. This deliberately avoids locale-dependent behavior and a Unicode case-folding dependency, but it means user-authored session titles and prompt labels with cased non-ASCII letters must use the same non-ASCII case to match.

## Full-screen selectable-list filtering

`SelectableList` remains the full-screen screen model for `/resume`, `/history`, and confirmation lists. Search-enabled lists add an owned `Search: <query>` row, query cursor, explicit result replacement transition, and query-specific empty state. Navigation and confirmation use the owned selected item ID; page and query replacement locate that identity in the new result set and otherwise select the first result deterministically. A page arriving without pending navigation preserves identity. A page requested by boundary Up, Down, PageUp, or PageDown completes that outstanding action exactly once after arrival.

`SelectableListDialog` owns the transition boundary between input and domain work. Text-changing edits request a complete result replacement; cursor-only edits redraw without loading. Boundary navigation requests the preceding or next page when its keyset continuation exists. The injected loader receives the current query, transition direction, and preferred selected identity for replacement. No loader runs from `frame()`.

The explicit states are:

- Initial/query loading retains query and prior selection while showing `Loading…`.
- Successful replacement discards every prior page and both continuation directions.
- Empty results own no selection, so Enter is inert.
- Query failure restores the prior usable query, cursor, pages, and selection, then shows a bounded error row.
- Page failure preserves loaded pages and selection and permits a retry.
- Esc cancels immediately, including while a load transition is active.
- Resize only reprojects the owned state.

`/history` filters the complete `conversation_checkpoints()` collection in memory at the input transition, before mapping matches to bounded visible rows. Its haystacks are the untruncated prompt label, checkpoint identity, task outcome, current/ancestor/preserved state text, total branch count, and representative leaf identities. Matching checkpoints retain their original tree order and stable checkpoint IDs.

`/resume` uses the catalog query/paging boundary described in `session-architecture.md`; it never filters the pages currently held by `SelectableList`.

## Model picker flow

`CompactPickerDialog` (`liminal/tui/compact_picker_dialog.h`) mirrors `SelectableListDialog`: `begin` opens the picker on the screen, `confirm`/`cancel` record a decision and close it, `next()` is the `Event`-gated awaitable consumed by `repl_body`.

Interactive `/model` with no arguments: open the picker in loading state → `guard_task(models.refresh())` → warnings render as ordinary transcript notices (visible above the band; picker state preserved) → a hard refresh error keeps prior catalog entries plus an error row → `model_picker_items` populates rows → await the decision. A confirmed row id is an exact catalog selector (`provider/id[@effort]`), so it maps back through `Catalog::select` and duplicate model IDs stay disambiguated by provider. Entries with declared efforts get one row per effort plus an explicit `@off` row (their bare selector resolves to the default effort, so "no effort" needs its own selector); the `@off` row carries the current marker when the active effort is null. Cancellation leaves the model and the transcript untouched — including Esc during the refresh, which cancels both the task and the picker silently (the notice is reserved for the non-picker selector path).

Non-interactive (redirected stdio) `/model` keeps the historical transcript catalog listing so scripted use keeps working. `/model <selector>` is unchanged.

## Empty-session hint

`frame()` writes `Ask Liminal anything. Type / for commands.` on the first viewport row only while the transcript has no blocks, the state is `EDITING`, and no compact surface is active. It is purely derived — no persistence flag — so any transcript content retires it naturally.

## Testing

Headless coverage lives in `liminal/tests/test-tui/main.cpp`: registry invariants, `/help`, compact and full-screen query editing, menu activation boundary and dismissal policy, identity-preserving replacement/paging, loading/empty/error/cancellation/resize states, semantic history filtering, model-picker flow, band shrink behavior, and both dialog protocols. `HeadlessSnapshot` exposes the active picker's query, cursor, identity, visible result IDs, loading state, and error for semantic assertions. PTY integration coverage includes `test_command_menu_and_model_picker` and `test_full_screen_resume_query_edit_and_clear` in `tests/integration/test_liminal.py`.
