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

## Discovery

Session discovery reads bounded, indexed catalog metadata without decoding entry payloads. Workspace association is discovery metadata, not an execution sandbox: the current invocation still controls the working directory and instruction discovery.

Interactive switching must preserve the current live session until the target lease, load, and transcript projection all succeed. Abandoning an unsaved tail requires an explicit user decision and does not imply that tool effects are reverted.

## Evolution constraints

- Persisted payload kinds and schema versions have explicit stable identities; C++ variant ordering is never a storage format.
- Schema migration is ordered and rejects databases that are newer, foreign, or unidentified and non-empty.
- New catalog features must remain independent of total payload size.
- New storage abstractions require a real second backend; speculative indirection is not a goal.
- Export, synchronization, encryption, cleanup, and repair are separate product decisions and must not introduce another canonical history store.
