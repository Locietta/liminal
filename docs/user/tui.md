# Interactive TUI

Liminal's interactive terminal interface keeps the transcript and prompt composer available at the same time. Tool activity updates in place, and long drafts can be handed to an external editor.

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
- `run_command` shows `Running <command>` while active and `Ran <command>` when finished, then reports its exit code, stdout/stderr line counts, and a bounded head/tail preview. Commands are highlighted as PowerShell on Windows and Bash on Linux, with executables and options visually separated from ordinary arguments; after ten seconds, a running command also shows live elapsed time.

Large results stay bounded, and separate parallel calls keep independent running and completion states.
The action and exact path or command stay bright, while completion metadata and output previews are dimmed for quick scanning.

## Keyboard shortcuts

| Shortcut             | Action                                                               |
| -------------------- | -------------------------------------------------------------------- |
| Enter                | Send the current prompt                                              |
| Ctrl+J / Shift+Enter | Insert a newline                                                     |
| Ctrl+G               | Edit the current draft in `VISUAL` or `EDITOR`                       |
| Up / Down            | Move within a multiline draft, then recall prompts at its boundaries |
| Ctrl+Up / Ctrl+Down  | Recall older or newer prompts directly                               |
| PageUp / PageDown    | Scroll the transcript by one viewport                                |
| Mouse wheel          | Scroll the transcript                                                |
