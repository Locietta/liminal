# TUI Interaction Design

This document records the durable interaction choices that shape Liminal's TUI. Detailed component and persistence contracts live in the linked developer documents; user-visible controls live in `docs/user/tui.md`.

## Product character

Liminal is a transcript-first, composer-primary terminal application. Its interface is keyboard-complete, mouse-optional, quiet while idle, and consistent across Windows and Linux. Controls are contextual, and status text describes only application or agent state that Liminal can observe.

The primary interaction is a focused agent conversation. Reading the transcript, editing the next prompt, and making a bounded selection are distinct interactions with predictable ownership of input.

## Surface hierarchy

The normal session has four stable regions:

1. A header identifying Liminal, the invocation workspace, and the session.
2. A transcript viewport containing conversation and tool activity.
3. The prompt composer.
4. A footer containing current model and capacity/status information.

Transcript browsing is part of normal session interaction. The composer stays editable, prompt history stays in the composer, and a semantic viewport anchor fixes the user's position after they leave the live tail.

Bounded contextual choices use one of two selection surfaces:

- Compact pickers render above the composer and preserve the normal session layout. Slash-command discovery and `/model` use this form.
- Full-screen pickers temporarily own input for workflows such as `/resume` and `/history`. Enter confirms and Esc restores the underlying session state.

Both forms preserve query and selected-item identity across result changes and resize. Loading, empty, error, success, and cancellation are explicit states; an error preserves the last usable state whenever possible.

## Commands and search

One command registry owns command names, aliases, descriptions, argument summaries, and availability metadata. Parsing, `/help`, and slash-command presentation derive from that registry, while execution guards remain authoritative in the dispatcher.

Command discovery uses predictable prefix matching. Model, resume, and conversation-history search use substring matching across their documented fields. `/resume` queries the complete current-workspace catalog through its paging boundary, and `/history` filters the complete semantic checkpoint collection before projecting bounded rows. Both operate on the complete logical dataset before bounded projection.

All picker search folds ASCII letters only. Non-ASCII UTF-8 text is accepted and searchable, but its bytes must match exactly: Liminal performs no non-ASCII case conversion and no Unicode normalization. User documentation states this contract wherever searchable fields can contain user-authored text.

Query editing is shared across picker forms. Cursor movement, deletion, and horizontal windowing respect extended grapheme clusters and terminal-cell width; pasted C0 controls are discarded.

## Session identity and responsive presentation

The normal header is `liminal · <workspace path> · <session title>`. The title resolves from an explicit `/name`, then a bounded first-prompt preview, then `New session`. Contextual full-screen pickers reuse the same semantic workspace and title state.

Workspace paths are interpreted independently of the host platform. Presentation tries a full path, then a fish-like path that shortens intermediate components, then terminal-cell truncation. Roots, separator convention, hidden-component dots, and the final component are preserved whenever the available width permits. Home substitution is component-aware.

Semantic header and footer models remain independent of terminal width. Resize reprojects them without changing session identity, picker state, draft state, or transcript position. Truncation never splits an extended grapheme cluster.

## Activity and footer semantics

The activity row reports observable lifecycle state:

- `Thinking…` while a task is waiting without visible assistant output or an active tool.
- `Writing…` while visible assistant text is streaming.
- `Running tool…` or `Running <n> tools…` while tools are active.

Tool activity takes precedence over writing, and lifecycle identities are scoped so repeated provider-local IDs and late events cannot corrupt the displayed state. Assistant progress is transcript content, while the activity row derives exclusively from lifecycle events.

The ordinary footer keeps this semantic order:

1. Model and optional reasoning effort.
2. Remaining context.
3. Cumulative token usage.
4. Optional authoritative provider limits.

When width is insufficient, whole segments are removed in the order token usage, remaining context, then provider limits. Model and effort are retained and truncated only as the final fallback. Critical guidance and failure states take precedence over ordinary metadata. Provider limits remain an optional presentation boundary and are absent unless authoritative data is available.

## Implementation and verification boundaries

- `SessionScreen` and picker models own semantic, width-independent state; renderers only project it.
- Frame construction performs no filesystem, environment, database, persistence, or provider work.
- Complete-dataset filtering and page loading occur only at explicit input/result transitions.
- Public TUI and application interfaces remain platform-neutral; native terminal, path, clipboard, and process details stay in implementation units.
- Layout, cursor movement, clipping, and truncation operate on terminal cells and complete grapheme clusters.
- Interaction behavior is verified primarily through the deterministic headless TUI adapter using semantic state and visible cells, with PTY integration coverage for end-to-end input paths.

Detailed contracts:

- `docs/dev/tui-command-registry-and-pickers.md`
- `docs/dev/tui-header-and-path-presentation.md`
- `docs/dev/tui-activity-and-footer-presentation.md`
- `docs/dev/session-architecture.md`
