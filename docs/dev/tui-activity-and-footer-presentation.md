# TUI Activity and Footer Presentation

## Scope and ownership

Phase 5 keeps semantic UI and viewport state in `SessionScreen`. `ConsoleRenderer` remains the only interactive output boundary. Activity and footer projection reuse the existing transcript layout, monotonic task timer, 100 ms animation refresh, shimmer, elapsed suffix, tail-following, and one-row viewport behavior.

`SessionScreen::frame()` is deterministic. It performs no filesystem, environment, database, persistence, or provider work. The width-independent `SessionFooter` model and observable lifecycle identities are application state; `present_footer()` returns a disposable terminal-cell projection.

## Observable activity state machine

Activity uses only application events that cross the semantic, platform-neutral event boundary. The agent assigns each task and provider call a monotonic `ActivityScope`; assistant output-item IDs and tool-call IDs are interpreted only inside that scope. The task generation remains stable across a tool loop, while the provider-call generation advances for every response. This is required because OpenAI-compatible streams may use provider-local fallback IDs such as `output:0` again in a later call.

| Observable state                                                            | Label                  |
| --------------------------------------------------------------------------- | ---------------------- |
| Active task with no streaming assistant item or active tool                 | `• Thinking…`          |
| At least one visible assistant text item is streaming and no tool is active | `• Writing…`           |
| Exactly one tool-call identity is active                                    | `• Running tool…`      |
| Two or more tool-call identities are active                                 | `• Running <n> tools…` |

Prompt submission begins at Thinking. An `AssistantTextDelta` activates its scoped output-item identity; `AssistantMessageCompleted` retires that identity. `ToolStarted` activates its scoped call identity, and the matching `ToolCompleted` retires only that call. Completing an assistant message without another streaming item returns the active task to Thinking, so message completion cannot leave a stale Writing label.

Tool activity has precedence over assistant streaming. Assistant identities continue to be tracked while tools run. When the last tool completes, the state becomes Writing if a visible assistant item remains active and Thinking otherwise. Parallel tool completion decrements the identity set without scanning rendered strings or hiding running siblings.

`ProviderActivityCompleted` retires one provider-call scope after its assistant items and tools have settled. Task completion, cancellation, failure, transcript replacement, and prompt submission clear active identities and retire the task generation. Because both generations are monotonic, `SessionScreen` retains only task and provider-call high-water marks; rejection of retired scopes is bounded O(1) and does not grow with transcript length. The reducer never consults historical transcript IDs to decide whether a new scoped identity is valid, so repeated IDs in a later provider call remain visible while late events from an earlier call cannot settle them.

The deterministic headless adapter owns equivalent monotonic generations. Submit begins a new task, assistant and tool actions use the current provider-call scope, `provider_activity_completed` emits the semantic retirement event and advances the next call, and completion/cancellation/failure close the task. This keeps multi-turn snapshots faithful even when item and call IDs are empty or repeat.

Successful `TaskCompleted` reduction enforces that no tool block remains running. It finalizes any residual streaming assistant text but never changes a tool block to successful; only the matching `ToolCompleted` event can establish a completed or failed tool outcome and summary. Cancellation and failure retain their existing explicit interrupted-tool settlement.

Assistant progress messages remain ordinary transcript blocks. The activity row describes lifecycle only and never replaces or derives state from progress text.

## Activity projection

The selected complete label passes through the existing grapheme-aware shimmer. The two-second cosine highlight and shared 100 ms timer are unchanged. After three seconds, the existing monotonic elapsed suffix is appended in the muted style and updates once per second. Successful completion still produces the persistent muted `Finished (Ns)` row; cancellation and failure do not.

Activity remains part of transcript flow. Tail-following prioritizes it at the newest output, transcript browsing scrolls it away, and a one-row transcript viewport keeps the status while dropping its trailing separator.

## Pure responsive footer projection

The ordinary semantic order is:

1. Model plus optional reasoning effort.
2. Remaining context.
3. Cumulative token usage.
4. Optional provider limits.

`present_footer()` constructs complete segments, measures terminal-cell width, and retains an exact fit. If the projection is oversized, it removes whole segments in this exact order:

1. Token usage.
2. Remaining context.
3. Provider limits.
4. Truncate model plus effort.

Model plus effort is therefore always the last ordinary segment retained. Separators are generated only between visible segments. Final truncation reserves one cell for an ellipsis and walks complete extended grapheme clusters, so combining sequences, wide characters, emoji variation sequences, and ZWJ emoji are never split.

## Provider-limit extension boundary

`SessionFooter::provider_limits` is an optional opaque display segment used only by the projector. An absent or empty value contributes no text, cells, spaces, or separator. Unit tests inject synthetic values to verify ordering and degradation.

No provider acquisition, API call, guessed capacity, refresh scheduling, aggregation, or provider-specific domain model is implemented. The authoritative data source, text, semantic color, time windows, reset presentation, and refresh contract remain deferred.

## Critical footer precedence

Critical states are selected before ordinary responsive metadata:

1. External-editor save-and-close guidance.
2. Transient confirmations such as clipboard status.
3. Persistent `SESSION NOT SAVING` failure.
4. Ordinary responsive metadata.

This preserves the established external-editor and transient-status relationship. When neither temporary override is active, persistence failure replaces ordinary metadata, so model/context/token fields cannot consume the row and hide the warning on narrow terminals.

## Verification contract

Tests inspect `SessionScreen` frames, activity spans, semantic styles, lifecycle scopes, and identity maps. They cover observable transitions, overlap precedence, concurrent count changes, repeated `output:0` IDs across provider calls, stale-event cleanup, the settled-tool completion invariant, shimmer and elapsed behavior for every label, completion/cancellation/failure behavior, browsing and one-row viewports, exact footer fit/degradation boundaries, provider absence, grapheme-safe truncation, separator integrity, and critical-status precedence.
