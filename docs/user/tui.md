# Interactive TUI

Liminal's interactive terminal interface keeps the transcript and prompt composer available at the same time. Long drafts can be handed to an external editor.

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

Liminal temporarily restores the terminal before launching the editor. The editor inherits standard input, output, error, and the current working directory. If configuration, launch, or editor exit fails, Liminal shows an error and preserves the original draft.

## Keyboard shortcuts

| Shortcut             | Action                                                       |
| -------------------- | ------------------------------------------------------------ |
| Enter                | Send the current prompt                                      |
| Ctrl+J / Shift+Enter | Insert a newline                                             |
| Ctrl+G               | Edit the current draft in `VISUAL` or `EDITOR`               |
| Up / Down            | Move within a multiline draft, then scroll at its boundaries |
| Ctrl+Up / Ctrl+Down  | Recall older or newer prompts                                |
| PageUp / PageDown    | Scroll the transcript by one viewport                        |
| Mouse wheel          | Scroll the transcript                                        |
