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

`MainWindow::start_ipc_server()` owns the POSIX Unix-domain stream server on
an independent `std::thread`. It reads requests, checks readability, replies,
and queues the document mutation with `QMetaObject::invokeMethod`. The reply
does not wait for GUI processing. The GUI mutates the workspace and increments
`workspace_revision_`, so remote documents invalidate an in-progress build like
local documents.

CR-1 wire protocol:

1. Client sends a four-byte big-endian network-order `uint32_t` payload length.
2. Payload bytes are exactly `ADD ` followed by an absolute UTF-8 path; spaces,
   non-ASCII characters, and newline are payload bytes rather than delimiters.
3. Server reads exactly the length (including across partial writes), then replies
   with bytes `OK` for a readable file or `ERR...` for rejection.

The server uses a Unix-domain stream socket and 50 ms shutdown polling.
`stop_ipc_server()` sets cancellation, shuts down the listener, joins the
server thread, and unlinks the socket.

## Fault switches

- `AIWS_FAULT_ACCEPT_STALE` disables the revision comparison at
  `MainWindow::poll_build_result()`; `stale_completion_is_not_published()` catches it.
- `AIWS_FAULT_LOSE_FAILURE` suppresses failure delivery in
  `MainWindow::poll_build_result()`; `build_failure_is_reported()` catches it.

Neither student_tests nor CMakeLists mentions either macro.

## AI use

AI assisted with implementation and testing; all requirements and decisions
were reviewed against the repository specification.

## Pair programming log

- **Partner:** Evan Cedar.
- **Session 1:** November 2, 2026, 2:15 PM-3:15 PM, working on Manan Sharma's code.
  We worked on the socket server, message format, remote document updates, and
  shutdown. We switched roles at 2:45 PM.
- **Navigator contribution by Manan:** noticed that socket messages may arrive
  in parts and suggested reading the full message before replying. He also
  suggested keeping workspace changes on the GUI thread.
- **Navigator contribution by Evan:** suggested checking server readiness and
  keeping socket cancellation separate from corpus-build cancellation.
- **Interaction that worked best:** working in short turns, then reviewing the
  socket code and shutdown logic together.
