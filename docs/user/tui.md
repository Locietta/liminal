# Interactive TUI

Liminal's interactive terminal interface keeps the transcript and prompt composer available at the same time. Tool activity updates in place, and long drafts can be handed to an external editor.

Assistant replies use a soft, normal-weight neutral foreground. Markdown headings and strong text become bold white, emphasis becomes italic without getting brighter, and links remain underlined and distinct. Blockquotes render with a green `│` gutter and green text while preserving inline code, links, bold, and italic styling. Code and diffs keep their existing semantic palette, while labels and secondary status details use a stable muted neutral rather than terminal dim text.

Prompts and replies render without `you:` or `assistant:` prefixes. Prompt color, reply typography, ordering, and tool/status rows keep turns visually distinct without redundant role labels.

## Header identity

The normal header shows `liminal · <workspace path> · <session title>`. An explicit `/name` title is used first, followed by a single-line
preview of the first prompt, then `New session`. Names and previews are made terminal-safe and shortened by complete displayed graphemes, so
multiline text, combining marks, wide characters, and joined emoji cannot corrupt or overflow the row.

The workspace is shown in full when it fits. Longer paths use fish-like shortening: intermediate components become their first grapheme,
hidden components retain their leading dot, and the final component remains whole whenever space permits. Tighter widths truncate safely by
terminal cells. Drive/filesystem roots and the path's slash style are preserved. If the workspace is the user's home directory or beneath it,
the matching home prefix is shown as `~`.

## Turn activity

While a turn is active, one status follows the newest output directly in the transcript:

- `• Thinking…` while Liminal waits for provider output.
- `• Writing…` while visible assistant text is arriving.
- `• Running tool…` while one tool is active.
- `• Running <n> tools…` while tools run concurrently.

Tool activity wins when text and tools overlap. When the last tool finishes, the status returns to Writing if assistant text is still arriving and otherwise to Thinking. A completed progress message also returns an active turn to Thinking instead of leaving a stale Writing status.

One blank line separates activity from the prompt composer. A soft highlight shimmers across the complete current label while its elapsed timer remains muted. When the turn completes, the line becomes a persistent muted `Finished (3s)` summary until the next turn begins and keeps the same trailing separation. Cancellation and failure do not retain that summary. Activity scrolls away naturally while browsing history. Scrolling does not replace footer metadata, and very short terminals drop the blank separator before hiding activity itself.

## Prompt composer

The draft appears inside a dark, padded input surface with a concise `›` marker. Multiline text aligns with the draft instead of the marker and remains vertically windowed around the cursor when it grows. The footer beneath the composer shows the selected model and effort, remaining context percentage, and cumulative tokens used. Each field has its own color so the metadata remains easy to scan.

On a narrow terminal, the footer removes whole fields in order: tokens first, context second, and an optional provider-limit field third. The model and effort remain last and are shortened by displayed terminal width only when nothing else remains. Separators disappear with their fields, and Unicode model names are never split inside a displayed character. Provider limits are shown only when an authoritative value has been supplied; Liminal does not currently acquire them.

External-editor guidance and brief confirmations replace ordinary metadata when they need attention. A persistence failure shows `SESSION NOT SAVING` instead of allowing narrow ordinary fields to hide it. On short terminals, decorative composer padding is removed before transcript or editing space.

## Slash commands

A new empty session shows a single muted hint — `Ask Liminal anything. Type / for commands.` — that disappears as soon as the session has any content.

Typing `/` at the start of the composer opens a compact command menu directly above the composer. It lists each command with its argument synopsis and a short description, filtered by prefix as you type; idle-only commands stay listed and are marked `(idle only)`. The menu only follows the command name: it closes when the cursor moves into the arguments, and a prompt that merely contains a slash later in its text never opens it. A draft starting with `//` is sent as an ordinary prompt beginning with `/`.

While the menu is open:

- Up / Down move the highlighted command.
- Tab completes the highlighted command name and keeps the menu open.
- Enter on an incomplete name completes it without executing; Enter on an exact command name (or alias) sends it as usual.
- Esc closes the menu without changing the draft; editing the command name reopens it.

`/help` prints a durable command reference into the transcript, listing every command with its arguments, aliases, and availability.

`/model` opens a compact model picker above the composer instead of printing the catalog. It shows one row per model and reasoning effort (plus an `@off` row for models with reasoning support), marks the current selection, and searches provider, model ID, display name, and effort labels as you type into its own query line, which supports the usual cursor, word, and deletion keys. Enter applies the highlighted model immediately; Esc cancels and leaves the current model unchanged. `/model <selector>` still selects directly. See `providers-and-models.md` for selector syntax.

## External prompt editor

Press Ctrl+G to open the current draft in the command configured by `VISUAL`. If `VISUAL` is not set, Liminal uses `EDITOR`. Save and close the editor to return the edited text to the composer; the draft is not sent automatically.

For the current PowerShell session:

```powershell
$env:VISUAL = "code --wait"
```

To persist that choice for future Windows sessions:

```powershell
[Environment]::SetEnvironmentVariable("VISUAL", "code --wait", "User")
```

For a Linux shell:

```bash
export VISUAL='code --wait'
```

Terminal editors can be configured directly, for example `VISUAL=nvim`. Graphical editors must include their wait option—such as `code --wait`—so Liminal knows when editing is finished. Editor commands may contain quoted arguments. Restart Liminal after changing a persistent environment variable.

Liminal keeps its current frame visible while temporarily restoring normal terminal input for the editor. The editor inherits standard input, output, error, and the current working directory. Terminal editors may draw over the frame while active; graphical editors leave it undisturbed. When the editor exits, Liminal redraws the frame and returns the edited draft to the composer. If configuration, launch, or editor exit fails, Liminal shows an error and preserves the original draft.

## Tool activity

Built-in tools show the consequential details in the transcript:

- `read_file` names the requested path, then reports line and byte counts.
- `apply_patch` reports the patch operation and a bounded list of changed paths.
- `exec_command` shows the command and returns either its exit status or a session ID. `write_stdin` sends characters to that session or polls it for incremental, bounded output. Commands are highlighted as PowerShell on Windows and Bash on Linux, with executables and options visually separated from ordinary arguments; after ten seconds, a running command also shows live elapsed time.
- Web search and page fetching run as hosted provider tools. Source links are surfaced while the answer streams and retained in the saved assistant text.

Large results stay bounded, and separate parallel calls keep independent running and completion states.
The action and exact path or command stay bright, while completion metadata and output previews are dimmed for quick scanning.

## Keyboard shortcuts

| Shortcut             | Action                                                                                     |
| -------------------- | ------------------------------------------------------------------------------------------ |
| Enter                | Send the current prompt; complete an incomplete highlighted command in the command menu    |
| Tab                  | Complete the highlighted command when the command menu is open; otherwise insert a tab     |
| Ctrl+J / Shift+Enter | Insert a newline                                                                           |
| Ctrl+G               | Edit the current draft in `VISUAL` or `EDITOR`                                             |
| Up / Down            | Move within a multiline draft, then recall prompts at its boundaries                       |
| Ctrl+Up / Ctrl+Down  | Recall older or newer prompts directly                                                     |
| PageUp / PageDown    | Scroll the transcript by one viewport                                                      |
| Mouse wheel          | Scroll the transcript                                                                      |
| Mouse drag           | Select visible text                                                                        |
| Esc                  | Close an open menu or picker first; otherwise cancel the active turn, preserving the draft |
| Ctrl+C               | Copy the selection when one is active; otherwise clear the draft, cancel the turn, or exit |
| Ctrl+O               | Copy the selection when one is active; otherwise copy the latest reply                     |

Drag selection highlights whole screen cells and persists after you release the button; nothing reaches the clipboard until you press Ctrl+C or Ctrl+O. In Windows Terminal, Ctrl+Shift+C also works because the terminal passes it through when it has no selection of its own. A plain click clears the selection, and it also clears when content changes, the transcript scrolls, or the terminal resizes, since the highlight is anchored to screen cells. Terminal-native selection remains available through the terminal's usual modifier (typically Shift+drag).

## Session discovery

Session catalog commands are available while the agent is idle:

- `/resume` opens recent sessions for the current workspace under `Resume Session · <workspace path>`. Its focused `Search:` row searches the complete workspace catalog by explicit title, first-prompt preview, or canonical full session UUID—even when a match is beyond the pages already shown. Up and Down move one row, PageUp and PageDown move between catalog pages, Enter confirms, and Esc cancels immediately without changing the current session.
- `/name <title>` names the current session. `/name --clear` returns it to the first-prompt preview.

In the `/resume` and `/history` search rows, printable text and paste insert at the cursor; pasted control characters such as trailing newlines and tabs are ignored. Backspace/Delete remove one grapheme, Ctrl+Backspace/Ctrl+Delete remove a word, Left/Right move by grapheme, Ctrl+Left/Ctrl+Right move by word, and Home/End move to the query boundaries. Clearing the query restores the ordinary ordering and paging. Matching ignores ASCII letter case but compares non-ASCII text exactly, including user-authored titles and prompt text.

Liminal fully prepares a selected resume target before replacing the current transcript. If the current session still has unsaved history after a final save attempt, Liminal asks before switching and explains that the unsaved tail will not be resumable and that external tool effects are not undone. Choosing to stay leaves the current session unchanged.

## Conversation history

`/history` is available while the agent is idle. It opens a tree of completed conversation checkpoints under
`Conversation History · <workspace path> · <session title>`. Rows identify the current append point, its active ancestors, preserved branches,
and stable checkpoint-based branch IDs. Its `Search:` row filters the complete checkpoint tree by the untruncated prompt label, full checkpoint
ID, task outcome, checkpoint state, branch count, and representative checkpoint/leaf IDs. Matching rows retain their original tree order. Up
and Down move between rows, Enter selects, and Esc cancels immediately.

After selecting a checkpoint, choose one of these actions:

- **Keep current session** returns without changing anything.
- **Checkout here** moves the current session's append point to that checkpoint. All later history remains preserved; the next prompt creates a new branch.
- **Fork from here** creates an independently durable session containing the selected history prefix, records its source, and switches to it.

Only coherent idle boundaries are selectable. Provider-call and tool lifecycle records are not shown as rewind points. An empty conversation reports that there are no completed checkpoints.

Checkout and fork change conversation context only. They do not undo file edits, commands, processes, network requests, or any other external tool effects. The action dialog states this before either operation.

Liminal does not add a prepared fork to `/resume` until the switch is authorized. If the source session cannot finish saving, a fork-specific warning explains that the selected prefix will be saved in the fork while unsaved source-only descendants or metadata may remain unavailable from the source. Choosing to stay cancels the fork without creating a session; explicit continuation publishes the fork and switches to it.
