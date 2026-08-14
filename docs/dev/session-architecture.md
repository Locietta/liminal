# Session Architecture

This document records the stable concepts behind Liminal sessions. It deliberately avoids concrete schemas and API signatures, which remain free to evolve with the implementation.

## Semantic authority

Liminal owns a provider-neutral semantic session. Provider request history, the terminal transcript, copyable replies, and catalog summaries are projections of that session rather than independent sources of truth.

A session records stable events, not streaming presentation updates. User task boundaries, completed provider output items, provider-call outcomes, tool-result batches, task outcomes, model preference changes, branch selection, and compaction checkpoints are semantic. Partial text deltas, timers, animations, scroll state, and drafts are not.

Provider-private items retain their provenance. A compatible adapter may replay them; an incompatible adapter preserves but omits them. Liminal does not invent a misleading provider-neutral interpretation.

## Durability and ownership

One local SQLite database is the canonical catalog and payload store. Catalog metadata and semantic entries commit together; there is no parallel JSONL authority or foreground scan-and-repair path.

The live session changes immediately, while persistence follows through an ordered queue. Storage failure must not block model calls or tools after their effects may already have occurred. The UI exposes an unsaved tail, and a later successful retry persists the complete ordered prefix.

Only one process may actively own a session. The active writer retains an operating-system lease and its durable revision, and persistence work remains bound to that session identity. Public interfaces stay platform-neutral even though lease implementations differ between Windows and POSIX systems.

## Recovery

Recovery resumes the last durable semantic prefix; it never reconstructs partial streamed text or automatically repeats a tool call.

An unmatched durable tool call receives a synthetic outcome-unknown result explaining that execution may have partially or fully occurred. Missing provider-call and task terminal records are completed with explicit interrupted outcomes. Existing terminal records remain authoritative, so recovery cannot turn a completed task into an interruption or discard its copyable reply.

No conversation operation claims to undo filesystem, process, network, or other external tool effects.

## History and projections

Entries form an append-only parent-linked history. Selecting an ancestor moves the active cursor; appending afterward creates another branch without deleting the original descendants. Compaction changes the active context projection while retaining earlier history.

Provider context is derived from the active branch according to provider compatibility. Transcript hydration is a separate non-streaming projection and must not recreate running states. A copyable reply comes only from the terminal provider call of a successfully completed task.

Conversation navigation exposes a distinct semantic checkpoint identity rather than accepting arbitrary entry IDs. A safe checkpoint is a completed task boundary or a compaction boundary whose ancestry is already idle. Provider output, provider-call, tool-result, and within-task compaction entries are not independently selectable. Projection derives checkpoint ancestry, descendants, active ancestry, the selected append point, and stable branch identity from the append-only entry tree; it does not persist parallel branch records.

Checkout is a session-domain cursor mutation. It validates the requested checkpoint, preserves every descendant, persists the selected append point, and then hydrates transcript and provider context from that ancestry. Appending after checkout creates a branch naturally. Conversation navigation never represents filesystem, process, network, or other external tool effects as reverted.

A fork is a new durable session containing the exact semantic prefix through a selected safe checkpoint. It records the source session and source checkpoint, receives fresh session identity and timestamps, starts unnamed and unarchived, and remaps entry, task, and provider-call identifiers into fork-local sequences. Provider output items, tool identifiers, compaction items, and provider-private payloads retain their semantic content and provenance. The fork commits to SQLite before it can become live and is prepared through the same transcript projection, model resolution, exclusive ownership, and unsaved-tail switch policy as a resumed session.

## Discovery

Session discovery reads bounded, indexed catalog metadata without decoding entry payloads. Workspace association is discovery metadata, not an execution sandbox: the current invocation still controls the working directory and instruction discovery.

Workspace catalogs are ordered newest first and traversed with stable keyset cursors rather than offsets. A catalog summary contains only bounded identification metadata; the full session ID remains its durable identity. An empty page is a successful discovery result, while failure to resolve an explicitly requested session is an error.

Catalog navigation uses a reusable focused selection surface. Commands supply domain data and cursor policy, but do not own terminal navigation or modal input handling.

Conversation navigation uses the same focused selection surface and is available only while the agent is idle. Cancellation, projection failure, persistence failure, fork preparation failure, and model-resolution failure must leave the live agent and displayed transcript on the same session and branch.

Target preparation has two ordered stages. Durable acquisition takes the exclusive lease, loads and recovers the session, attaches persistence, and projects the transcript without depending on provider discovery. Model resolution then consults the current catalog and policy, so startup claims a requested session before discovery and interactive switching cannot retain a stale fallback.

Interactive switching must preserve the current live session until both preparation stages succeed. Abandoning an unsaved tail requires an explicit user decision and does not imply that tool effects are reverted.

Naming and archive state are ordinary session mutations: they advance session metadata and follow the same ownership and persistence rules as semantic history. Archiving changes discovery visibility without deleting history, and mutating an inactive session first requires exclusive ownership.

## Evolution constraints

- Persisted payload kinds and schema versions have explicit stable identities; C++ variant ordering is never a storage format.
- Schema migration is ordered and rejects databases that are newer, foreign, or unidentified and non-empty.
- New catalog features must remain independent of total payload size.
- New storage abstractions require a real second backend; speculative indirection is not a goal.
- Export, synchronization, encryption, cleanup, and repair are separate product decisions and must not introduce another canonical history store.
