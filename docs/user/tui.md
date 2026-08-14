# Interactive TUI

Liminal's interactive terminal interface keeps the transcript and prompt composer available at the same time. Tool activity updates in place, and long drafts can be handed to an external editor.

Assistant replies use a soft, normal-weight neutral foreground. Markdown headings and strong text become bold white, emphasis becomes italic without getting brighter, and links remain underlined and distinct. Blockquotes render with a green `│` gutter and green text while preserving inline code, links, bold, and italic styling. Code and diffs keep their existing semantic palette, while labels and secondary status details use a stable muted neutral rather than terminal dim text.

Prompts and replies render without `you:` or `assistant:` prefixes. Prompt color, reply typography, ordering, and tool/status rows keep turns visually distinct without redundant role labels.

## Turn activity

While a turn is active, `• Working…` follows the newest output directly in the transcript, with one blank line after it separating turn activity from the prompt composer. A soft highlight shimmers across the label while its elapsed timer remains muted. When the turn completes, the line becomes a persistent muted `Finished (3s)` summary until the next turn begins and keeps the same trailing separation. It scrolls away naturally while browsing history. Scrolling does not replace the footer's model, workspace, context, and token metadata. Very short terminals drop the blank separator before hiding the activity itself.

## Prompt composer

The draft appears inside a dark, padded input surface with a concise `›` marker. Multiline text aligns with the draft instead of the marker and remains vertically windowed around the cursor when it grows. The footer beneath the composer shows the selected model and effort, workspace path, remaining context percentage, and cumulative tokens used. Each field has its own color so the metadata remains easy to scan; browsing and external-editor states replace it when they need attention. On short terminals, decorative padding is removed before transcript or editing space.

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
| Enter                | Send the current prompt                                                                    |
| Ctrl+J / Shift+Enter | Insert a newline                                                                           |
| Ctrl+G               | Edit the current draft in `VISUAL` or `EDITOR`                                             |
| Up / Down            | Move within a multiline draft, then recall prompts at its boundaries                       |
| Ctrl+Up / Ctrl+Down  | Recall older or newer prompts directly                                                     |
| PageUp / PageDown    | Scroll the transcript by one viewport                                                      |
| Mouse wheel          | Scroll the transcript                                                                      |
| Mouse drag           | Select visible text                                                                        |
| Esc                  | Cancel the active turn while preserving the current draft                                  |
| Ctrl+C               | Copy the selection when one is active; otherwise clear the draft, cancel the turn, or exit |
| Ctrl+O               | Copy the selection when one is active; otherwise copy the latest reply                     |

Drag selection highlights whole screen cells and persists after you release the button; nothing reaches the clipboard until you press Ctrl+C or Ctrl+O. In Windows Terminal, Ctrl+Shift+C also works because the terminal passes it through when it has no selection of its own. A plain click clears the selection, and it also clears when content changes, the transcript scrolls, or the terminal resizes, since the highlight is anchored to screen cells. Terminal-native selection remains available through the terminal's usual modifier (typically Shift+drag).

## Session discovery

Session catalog commands are available while the agent is idle:

- `/resume` opens recent, non-archived sessions for the current workspace. Up and Down move one row, PageUp and PageDown move between catalog pages, Enter confirms, and Esc cancels without changing the current session.
- `/name <title>` names the current session. `/name --clear` returns it to the first-prompt preview.
- `/archive` opens the active-session catalog and archives the selected session without deleting it.
- `/unarchive` opens the archived-session catalog, so every archived session remains identifiable and restorable.

Liminal fully prepares a selected resume target before replacing the current transcript. If the current session still has unsaved history after a final save attempt, Liminal asks before switching and explains that the unsaved tail will not be resumable and that external tool effects are not undone. Choosing to stay leaves the current session unchanged.

## Conversation history

`/history` is available while the agent is idle. It opens a tree of completed conversation checkpoints. Rows identify the current append point, its active ancestors, preserved branches, and stable checkpoint-based branch IDs. Up and Down move between rows, Enter selects, and Esc cancels.

After selecting a checkpoint, choose one of these actions:

- **Keep current session** returns without changing anything.
- **Checkout here** moves the current session's append point to that checkpoint. All later history remains preserved; the next prompt creates a new branch.
- **Fork from here** creates an independently durable session containing the selected history prefix, records its source, and switches to it.

Only coherent idle boundaries are selectable. Provider-call and tool lifecycle records are not shown as rewind points. An empty conversation reports that there are no completed checkpoints.

Checkout and fork change conversation context only. They do not undo file edits, commands, processes, network requests, or any other external tool effects. The action dialog states this before either operation.

Liminal does not add a prepared fork to `/resume` until the switch is authorized. If the source session cannot finish saving, a fork-specific warning explains that the selected prefix will be saved in the fork while unsaved source-only descendants or metadata may remain unavailable from the source. Choosing to stay cancels the fork without creating a session; explicit continuation publishes the fork and switches to it.
