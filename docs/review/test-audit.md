# Test audit (liminal, 2026-10-12)

Each test function (216 in 10 unit targets plus 17 integration tests) was judged
on whether it would fail for a plausible real regression in liminal. Temporary
working notes; delete once the items below are done or tracked elsewhere.
Counts: 198 KEEP, 15 TRIM, 2 MERGE, 1 REMOVE. The suite is strong overall; the
weak spots are a few brittle constant checks, some sleep-based shell tests, and
cancellation / session-switching paths with no test.

## Applied

| test                                                                        | change                         | why                                                                                                                               |
| --------------------------------------------------------------------------- | ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------- |
| `test-session-store` `test_removed_archive_surface_and_state_root_override` | removed                        | stale name; only checked that `state_root_path()` reads `$LIMINAL_STATE_DIR`, which every integration test relies on              |
| `test-tools` `test_tools_are_available_by_default`                          | trimmed                        | looks tools up by name instead of asserting exactly 7 in a fixed order, which broke with every new tool                           |
| `test-agent` `test_successful_task`                                         | trimmed                        | dropped literal prompt-text checks (`` `rg --files` ``, `uutils`, `` `apply_patch` ``)                                            |
| `test-provider-anthropic` `test_request_encoding`                           | trimmed                        | dropped the literal instruction-hierarchy preamble check; role labelling is still checked                                         |
| `test-provider-*` `main()`                                                  | fixed                          | failures now print and return 1 like every other target instead of `std::terminate`; all tests moved into the anonymous namespace |
| `test-tui` command checks                                                   | trimmed                        | dropped assertions that removed commands (`/archive`, `/unarchive`, `/queue`) stay removed                                        |
| `test-tui` `check_command_menu_rows_and_availability`                       | trimmed                        | compares menu rows against `find_command(...)->description` instead of literal descriptions                                       |
| `test-tui` `check_rich_output_and_concurrent_tools`                         | moved (in the lilac migration) | per-language token roles and aliases are tested in lilac; liminal keeps the role→style mapping, fallback and line guard           |
| integration `stream_paused.wait(1)` (4 places)                              | raised to 5 s                  | matches the bound used elsewhere; 1 s was a flake risk on slow machines                                                           |

## Recorded, not applied

Changes that need a redesign rather than a deletion:

- **`test-tools` `test_distinct_shell_tasks_interact_concurrently` cannot catch
  the regression it names**, and its barriers are `sleep 0.01` loops. If
  writes were serialized, write-a would return after its 2000 ms
  `yield_time_ms` and b would still create `b.ready` inside a's 5 s window.
  Fix: give write-a a long yield, wait without a timeout in the script, and
  assert write-a's response already contains `task-a`.
- **`test-tools` `test_shell_session_eof_and_kill`** polls `process_terminated`
  with `sleep(25ms)` × 200, against the style guide. Use pidfd on Linux, or
  have the grandchild hold an inherited pipe or lock and wait for EOF.
- **Integration `test_terminal_session_restores_state`** polls
  `os.waitpid(WUNTRACED | WNOHANG)` with `time.sleep(0.01)`; a blocking
  `os.waitpid(pid, os.WUNTRACED)` is deterministic. Needs a Linux run to verify.
- **`test-tui` `check_surface_cells_and_encoding`** asserts exact RGB escape
  codes for the palette, so every theme change edits the test. Replace with
  invariants: every `Style` encodes a distinct SGR, no style emits `\x1b[2m` or
  `\x1b[90m`, diff and footer styles use truecolor.
- **`test-tui` `check_rich_output_and_concurrent_tools`**: the remaining C++
  role checks are the only coverage of `style_for_role`
  (`tui/syntax_highlight.cpp`). Expose it and test the table directly, then drop
  them. The Windows/POSIX shell rows check `Select-Object` / `head`, which both
  lexers classify the same way; use a PowerShell- or Bash-only token to prove
  the right lexer was picked.
- **`test-tui` `check_command_menu_key_semantics`** hard-codes
  `{"context", "compact", "copy"}` for `/c`; derive it from `command_registry()`.
- **`test-session-store` `test_catalog_keyset_paging_and_index_plan`** runs
  its own copy of the paging SQL under `EXPLAIN QUERY PLAN`; the real query is
  built at `session/catalog.cpp:635` and can drift. Kept because it still
  catches a dropped `sessions_workspace_recent` index; share the SQL to make it exact.
- **Merges declined:** `test_independent_session_write_transactions` still fails
  if two sessions share a database file; transcript
  `check_repeated_item_ids_are_provider_scoped` is a unit-level version of the
  TUI check and cheap to keep.
- **Small leftovers:** `test_context_manifest` (test-agent) belongs in
  test-context; `test-model` `run_all` holds an unnamed registry-parsing block
  that should become a named test; integration
  `test_steering_edits_stay_in_active_task` still sends `/queue` as an unknown
  command mid-task.

## Missing tests

| #   | intention without a test                                                                                                               | where                                                           | P   |
| --- | -------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------- | --- |
| 1   | Cancelling while tools run: started calls get `OUTCOME_UNKNOWN`, queued calls `NOT_STARTED`, session validates                         | `agent.cpp:395-415`, `tool_executor.cpp:213-241`                | P1  |
| 2   | Live exec process trees are killed when the ToolSet is destroyed                                                                       | `exec.cpp:242-248`                                              | P1  |
| 3   | Switching away from an unsaved session: flush failure asks; STAY cancels; redirected mode auto-STAYs                                   | `session_coordinator.cpp:78-94`, `session_commands.cpp:200-216` | P1  |
| 4   | Declining the unsaved-fork confirmation publishes nothing; a dropped `ForkPlan` leaves no catalog entry                                | `session_coordinator.h:29-30`, `session_commands.cpp:244-249`   | P1  |
| 5   | `/history` checkout through the real command path restores the cursor on flush failure                                                 | `session_commands.cpp:344-372`                                  | P1  |
| 6   | apply_patch guards: no Add over an existing file, no Move onto one, no path touched twice, binary rejected, nothing written on failure | `apply_patch.cpp:209-386`                                       | P1  |
| 7   | An SSE stream ending before `message_stop` / `response.completed` fails instead of reporting DONE                                      | `anthropic.cpp:840-850`, `openai.cpp:847-856`                   | P1  |
| 8   | A `codex` providers.json entry cannot override `api` / `base_url` / `api_key` (OAuth token leak)                                       | `registry.cpp:258-265`                                          | P1  |
| 9   | Cancelling a manual `/compact` leaves history unchanged                                                                                | `repl.cpp:692-705`                                              | P1  |
| 10  | A transport failure after visible text is not retried by `attempt_stream`                                                              | `anthropic.cpp:836-838`, `openai.cpp:843-845`                   | P2  |
| 11  | Non-SSE `content-type` rejected; only Codex may omit it                                                                                | `openai.cpp:832-836`, `registry.cpp:235`                        | P2  |
| 12  | `$VAR` api_key resolution and the unset-variable error                                                                                 | `registry.cpp:149-160`                                          | P2  |
| 13  | Registry validation: duplicate model id, bad default effort, `"off"` / empty effort, unknown key, empty `base_url`                     | `registry.cpp:67-122, 289-291`                                  | P2  |
| 14  | Codex override merges into bundled models; merged capacity validated                                                                   | `registry.cpp:114-147, 276-283`                                 | P2  |
| 15  | `make_choice`: Codex omits `max_output_tokens`, Anthropic gets `max_tokens`                                                            | `registry.cpp:222-247`                                          | P2  |
| 16  | Model discovery over HTTP for anthropic and openai                                                                                     | `anthropic.cpp:959-`, `openai.cpp:1039-1090`                    | P2  |
| 17  | Model precedence: `LIMINAL_MODEL` over stored preference; vanished stored model falls back with a notice                               | `session_coordinator.cpp:29-56`                                 | P2  |
| 18  | `@off` clears the effort                                                                                                               | `catalog.cpp:111`                                               | P2  |
| 19  | `liminal resume <prefix>`: length bounds, dashes, ambiguous / not found                                                                | `catalog.cpp:549-579`, `main.cpp:101-108`                       | P2  |
| 20  | Workspace key is canonical and case-insensitive on Windows                                                                             | `paths.cpp:56-70`, `paths_win.cpp`                              | P2  |
| 21  | REPL key routing (Ctrl+J, Shift+Enter, Ctrl+Up/Down, AltGr, Ctrl+G/O, Esc, wheel)                                                      | `repl.cpp:191-346`                                              | P2  |
| 22  | Ctrl+C with a selection copies instead of cancelling                                                                                   | `repl.cpp:507-533`                                              | P2  |
| 23  | "Session not saving" / catalog-pending notices once per episode, re-armed after recovery                                               | `repl.cpp:601-619`                                              | P2  |
| 24  | `/name` reports saved / saving failed / catalog pending                                                                                | `session_commands.cpp:312-322`                                  | P2  |
| 25  | External editor: `VISUAL` over `EDITOR`, neither set errors, non-zero exit keeps the draft                                             | `external_editor.cpp:232-262`                                   | P2  |
| 26  | Selecting the current session in `/resume` reports it                                                                                  | `session_coordinator.cpp:251`                                   | P3  |
| 27  | Prompts: tool names in `runtime-tools.md` are registered tools; defaults reach a request end to end                                    | `prompts/`, `main.cpp:223-243`                                  | P3  |
| 28  | Redirected stdin strips CRLF and submits a final unterminated line                                                                     | `repl.cpp:145-188`                                              | P3  |
| 29  | Footer context percentage clamps at 0                                                                                                  | `repl.cpp:68-84`                                                | P3  |
| 30  | CLI: unknown arguments exit 2; no models exits 1                                                                                       | `main.cpp:208-212, 288-299`                                     | P3  |
