# M4 Design Note

## Components and threads

`MainWindow` remains the GUI-thread coordinator and owns the `Workspace`, all
widgets, corpus state, and the currently published `ProcessingCore`. On Build
Corpus, it copies the workspace and the per-document delay into a build request
with the current workspace revision. A single `std::thread` builds a separate
`ProcessingCore` from that immutable snapshot. The worker never reads the live
workspace or accesses a widget. It returns a result or an error through
`QMetaObject::invokeMethod` with `Qt::QueuedConnection`; only the GUI-thread
completion handler may publish the result or update the interface. The thread
owner joins the worker when completion is handled and during window teardown.
Build Corpus is unavailable while a worker is active, preventing overlapping
workers; other workspace and GUI actions remain available. A wrapper around
the normal chunking strategy applies the requested per-document delay in
interruptible intervals and checks cancellation between documents.

The only mutable value shared between threads is a cancellation flag: the GUI
may set it during shutdown and the worker may read it during the delay and
between documents. The workspace snapshot, revision number, delay, built core,
and error are transferred as request/result values, not concurrently shared.
`std::thread` itself is owned and joined by the GUI-side coordinator. After a
confirmed Exit, the worker is asked to stop; teardown joins it, allowing its
current document operation to finish but skipping remaining documents and
delay.

## Corpus states and transitions

The corpus states are **Needs rebuild**, **Building**, and **Current**. Adding
a document or Reset increments the workspace revision,
invalidates the corpus, clears dependent results, and shows **Needs rebuild**.
Starting a build captures that revision and shows **Building**. On completion,
the GUI accepts a successful core and displays its chunk count only when the
request revision still equals the live revision. A mismatch discards the core
and leaves the state at **Needs rebuild**. A matching failure is reported to
the user and leaves the application usable in **Needs rebuild**. Search, Term
Inspector, and Context use only the published core while its state is
**Current**.

## Risky event sequences

1. **Workspace changes during a build (R1/R2):** Build starts at revision 4;
   the user adds a document, advancing the revision to 5 and invalidating the
   corpus while the worker continues on its revision-4 copy. When that result
   arrives, the GUI compares revisions, discards it, and never shows its count
   as current. The user can build revision 5 after the first worker completes.
2. **Reset or Exit during a delayed build (R1/R4):** Reset advances the
   revision, clears the UI, and leaves the worker unable to publish its old
   result. On confirmed Exit, the owner requests cancellation; the worker
   finishes at most its current document operation, skips remaining documents,
   and is joined before destruction. No thread is detached and no callback can
   use a destroyed window.
3. **Build failure (R3):** A worker exception is carried as an error result to
   the GUI thread, where it is shown to the user; the current corpus remains
   invalid and normal editing and retry remain available.
4. **Repeated build requests (R4):** Only one worker may run at a time; the
   build action is disabled until its completion has been joined, so repeated
   use cannot accumulate worker threads or retained cores.

## Corpus ownership and rejected alternative

The worker creates and exclusively owns its new `ProcessingCore` while
rebuilding. On successful delivery, ownership moves in the queued result and
then to `MainWindow` as the published core. A rejected stale result is
destroyed without publication. Reset/invalidation makes the old core
unreachable by processing actions; it is destroyed when replaced or when the
window is destroyed. I reject rebuilding the GUI's published core directly in
the worker: it would expose mutable chunks and indexes to concurrent GUI
queries and could publish a corpus for a workspace that has already changed.
