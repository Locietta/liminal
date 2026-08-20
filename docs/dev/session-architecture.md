# Session Architecture

This document records the stable concepts behind Liminal sessions. It deliberately avoids concrete schemas and API signatures, which remain free to evolve with the implementation.

## Semantic authority

Liminal owns a provider-neutral semantic session. Provider request history, the terminal transcript, copyable replies, and catalog summaries are projections of that session rather than independent sources of truth.

A session records stable events, not streaming presentation updates. User task boundaries, completed provider output items, provider-call outcomes, tool-result batches, task outcomes, model preference changes, branch selection, and compaction checkpoints are semantic. Partial text deltas, timers, animations, scroll state, and drafts are not.

Provider-private items retain their provenance. A compatible adapter may replay them; an incompatible adapter preserves but omits them. Liminal does not invent a misleading provider-neutral interpretation.

## Durability and ownership

Each published session owns one authoritative SQLite database under its deterministic, validated session-ID path. Its singleton row and append-only entry tree commit together. Separate sessions use separate connections, mutexes, and SQLite writer locks, so normal history writes never serialize through a process-global store.

A small global SQLite catalog is a disposable discovery projection. It contains only session ID, observed authoritative revision, workspace key, conversation recency, title, and bounded initial-prompt preview. Exact full-ID acquisition derives the session path directly and does not depend on this catalog.

The live session changes immediately, while semantic persistence follows through an ordered per-session queue. Storage failure must not block model calls or tools after their effects may already have occurred. The UI exposes an unsaved tail, and a later successful retry persists the complete ordered prefix. Publication and rename make a bounded synchronous catalog attempt after the authoritative transaction, so catalog contention may delay their completion report without affecting durability. A separate catalog indexer coalesces subsequent projection requests and retries them independently. Catalog refresh failure is reported separately and cannot turn a successful authoritative commit into semantic persistence failure.

Only one process may actively own a session. The active writer retains an operating-system lease and its durable revision, and persistence work remains bound to that session identity. Public interfaces stay platform-neutral even though lease, durable marker, and publication implementations differ between Windows and POSIX systems.

Payload validation and encoding occur before a SQLite write transaction. A session commit compares and advances the singleton revision atomically with its already-encoded entry append and metadata update.

## Recovery

Recovery resumes the last durable semantic prefix; it never reconstructs partial streamed text or automatically repeats a tool call.

An unmatched durable tool call receives a synthetic outcome-unknown result explaining that execution may have partially or fully occurred. Missing provider-call and task terminal records are completed with explicit interrupted outcomes. Existing terminal records remain authoritative, so recovery cannot turn a completed task into an interruption or discard its copyable reply.

No conversation operation claims to undo filesystem, process, network, or other external tool effects.

## History and projections

Entries form an append-only parent-linked history. Selecting an ancestor moves the active cursor; appending afterward creates another branch without deleting the original descendants. Compaction changes the active context projection while retaining earlier history.

Provider context is derived from the active branch according to provider compatibility. Transcript hydration is a separate non-streaming projection and must not recreate running states. A copyable reply comes only from the terminal provider call of a successfully completed task.

Conversation navigation exposes a distinct semantic checkpoint identity rather than accepting arbitrary entry IDs. A safe checkpoint is a completed task boundary or a compaction boundary whose ancestry is already idle. Provider output, provider-call, tool-result, and within-task compaction entries are not independently selectable. Projection derives checkpoint ancestry, descendants, active ancestry, the selected append point, and stable branch identity from the append-only entry tree; it does not persist parallel branch records. Descendant branch summaries retain a total leaf count and bounded representative identities rather than duplicating every leaf into every ancestor.

Checkout is a session-domain cursor mutation. It validates the requested checkpoint, preserves every descendant, persists the selected append point, and then hydrates transcript and provider context from that ancestry. Appending after checkout creates a branch naturally. Conversation navigation never represents filesystem, process, network, or other external tool effects as reverted.

A fork is a new durable session containing the exact semantic prefix through a selected safe checkpoint. It records the source session and source checkpoint, receives fresh session identity, starts unnamed, and remaps entry, task, and provider-call identifiers into fork-local sequences. Provider output items, tool identifiers, compaction items, and provider-private payloads retain their semantic content and provenance. Its conversation recency is the publication time.

Fork preparation is not publication. A disposable fork plan owns the new identity's exclusive lease, rollback-journal staging database, unpublished persistence queue, projected transcript, and resolved model without creating a catalog row. Cancellation removes the staging directory. After the source saves successfully or the user explicitly accepts its remaining unsaved state, publication closes the staged database, creates its catalog-pending marker, atomically moves the directory without replacement, reopens the published database in verified WAL mode, and then attempts catalog projection. Once the atomic move succeeds, the queue retains that exact finalized snapshot until attachment completes; a fork may replace the live session with an explicit persistence-degraded notice while this recovery remains pending. If the source save is explicitly abandoned, the origin remains exact semantic provenance and may identify a checkpoint that never became durable in the source session.

## Discovery

Session discovery reads bounded, indexed catalog metadata without decoding entry payloads. Workspace association is immutable session metadata used for discovery, not an execution sandbox: the current invocation still controls the working directory and instruction discovery.

Workspace catalogs are ordered newest first and traversed with stable keyset cursors rather than offsets. A catalog summary contains only bounded identification metadata; the full session ID remains its durable identity. An empty page is a successful discovery result, while failure to resolve an explicitly requested session is an error.

The catalog paging request also carries an optional platform-neutral text query. An empty query uses the ordinary workspace/recency keyset query. A non-empty query filters inside SQLite across explicit title, bounded first-prompt preview, and the canonical full UUID while retaining the same workspace predicate, `updated_at_ms DESC, id DESC` order, bounded page size, and keyset continuation. Matching uses literal `instr` operations, so `%`, `_`, and similar characters have no wildcard meaning. ASCII letters are compared case-insensitively; non-ASCII title and preview bytes must match exactly.

Replacement may include a preferred session ID. The catalog checks that exact ID under the same workspace and text predicates and, when it still matches, returns a bounded page beginning at that identity plus preceding and following keyset continuations. This keeps the preferred row selected without loading or walking every newer match; PageUp/Up can traverse newer pages and PageDown/Down can traverse older pages while every loaded page remains in global recency/ID order. A missing or nonmatching preferred ID falls back to the ordered first page. Every continuation belongs to one query, and changing or clearing the query discards both directions before establishing the replacement page.

Catalog navigation uses a reusable focused selection surface. Commands supply domain data and cursor policy, but do not own terminal navigation or modal input handling. Search result replacement and page loading happen only at input/result transitions; frame projection never performs catalog work and the picker never eagerly loads the complete catalog.

Conversation navigation uses the same focused selection surface and is available only while the agent is idle. Cancellation, projection failure, persistence failure, fork preparation failure, and model-resolution failure must leave the live agent and displayed transcript on the same session and branch.

Target preparation has two ordered stages. Durable acquisition takes the exclusive lease, validates the deterministic database identity and immutable workspace association, loads and recovers the session, attaches persistence, and projects the transcript without depending on provider discovery. Model resolution then consults the current model catalog and policy, so startup claims a requested session before discovery and interactive switching cannot retain a stale fallback. A selected discovery row remains a hint until these checks complete.

Interactive switching must preserve the current live session until both preparation stages succeed. Abandoning an unsaved tail requires an explicit user decision and does not imply that tool effects are reverted. Fork confirmation is distinct from resume confirmation because the selected prefix will be saved in the fork even when unsaved source-only history or metadata cannot be saved in the source.

Naming is an authoritative session mutation and follows the same ownership and persistence rules as semantic history. It does not advance conversation recency. After its session commit, Liminal makes a bounded synchronous catalog-refresh attempt so selectors normally reflect the new title immediately.

There is no archived or otherwise hidden session state: every published session remains resumable and participates in ordinary workspace discovery until a future explicit deletion removes it. Conversation recency advances non-regressingly when a user task is admitted and becomes durable atomically with that task start. Output, tools, task completion, recovery bookkeeping, model changes, rename, checkout, compaction, catalog repair, and SQLite maintenance do not advance it.

## Catalog recovery

Before publication, rename, or durable user-task admission, Liminal durably replaces `catalog-pending/<session-id>` with the target authoritative revision. After the session commit, the projection reads the latest singleton row, performs a revision-guarded catalog upsert, and removes the marker only if it still names no later revision. The session-local invalidation lock covers only marker comparison, replacement, and conditional removal; authoritative reads and catalog transactions never hold it or delay semantic commits.

Normal startup inspects pending markers and the bounded staging directory. It tries each relevant session lease without waiting, preserves work owned by another process, and removes abandoned staging before clearing its orphan marker. Missing-catalog rebuild and explicit repair may scan published session directories, but use validated read-only connections to read singleton rows alongside live WAL writers and never decode history payloads. Before a new catalog schema becomes valid, Liminal durably creates `catalog-rebuild-pending`; one repository opener serializes and completes the rebuild before durably removing that marker, while contending openers remain available in authority-only mode. A crash or rebuild error therefore leaves an unambiguous instruction for the next opener. Creating or rebuilding a missing catalog retains exclusive catalog-maintenance ownership until discovery has been reconstructed, so a POSIX process cannot replace a catalog while another process still writes an unlinked open handle. Corrupt replacement follows the same exclusive rule.

## Evolution constraints

- Persisted payload kinds and schema versions have explicit stable identities; C++ variant ordering is never a storage format.
- Schema migration is ordered and rejects databases that are newer, foreign, or unidentified and non-empty.
- New catalog features must remain independent of total payload size.
- New storage abstractions require a real second backend; speculative indirection is not a goal.
- Export, synchronization, encryption, cleanup, and repair are separate product decisions and must not introduce another canonical history store.
