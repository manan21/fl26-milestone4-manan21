# M4 Decision Record

## Threads and ownership

`MainWindow` owns the GUI thread, live `Workspace`, published `ProcessingCore`,
and one joinable `std::thread` build worker. The worker receives an immutable
workspace copy and constructs a separate `ProcessingCore` from a delayed
chunking strategy. Build completion is represented by a `std::future`; a
25 ms Qt timer polls it on the GUI thread. Widgets and the published corpus are
therefore touched only on the GUI thread. Exit requests cancellation, and the
window destructor/join path joins every worker before clearing ownership.

The `BuildState::cancelled` shared atomic is the only application data mutable
by both threads: the GUI writes it on cancellation and the worker reads it in
delay intervals and before remaining documents. It is safe because
`std::atomic<bool>` supplies its synchronization. The workspace snapshot,
future/promise transfer, request revision, delay, result core, and error are
accessed by one thread at a time; the standard library synchronizes promise
completion with future readiness. No `ProcessingCore` or widget is shared.

## Stale-build validity

`MainWindow::add_document_file()` and Reset increment `workspace_revision_`.
Each build records the revision and captures the workspace. Completion is
accepted only when the recorded revision equals the current revision. Stale
results are discarded in `MainWindow::poll_build_result()`, and the workspace
is left needing a rebuild.

## Changes since the checkpoint

The checkpoint planned cancellation as current-document completion followed by
joining; the implementation additionally cancels delay intervals in 10 ms
steps. The completion mechanism is `std::future` plus a GUI timer rather than
queued invocation, allowing the worker to remain wholly free of Qt objects.

## CR-1

No CR-1 specification has been provided in this repository, so no external
event source is implemented yet. The GUI-thread revision invalidation API and
completion boundary are isolated in `MainWindow`, making later CR-1 events able
to invalidate the workspace without changing worker/build ownership.

## Fault switches

- `AIWS_FAULT_ACCEPT_STALE` disables the revision comparison at
  `MainWindow::poll_build_result()`; `stale_completion_is_not_published()` catches it.
- `AIWS_FAULT_LOSE_FAILURE` suppresses failure delivery in
  `MainWindow::poll_build_result()`; `build_failure_is_reported()` catches it.

Neither student_tests nor CMakeLists mentions either macro.

## AI use

AI assisted with implementation and testing; all requirements and decisions
were reviewed against the repository specification.
