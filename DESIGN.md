# M5 Design Record

## M4 fixes

CR-1 now binds before reporting server startup success, reads partial requests
with a deadline, and can stop while a client is mid-request. `aiws_send` checks
the result of nonblocking `connect` rather than stale `errno`. M4 cancellation
could finish the current delay but still chunk every remaining document; the
M5 worker now abandons the build when its request is invalidated.

## Threads and shared data

The GUI thread owns all widgets, the workspace, corpus states, and the published
`ProcessingCore`. One persistent build worker waits for requests and builds only
immutable workspace snapshots. The CR-1 server has a separate `std::thread`.
The build worker and GUI share `pending_build_`, `build_generation_`,
`worker_busy_`, and `worker_stopping_`, all protected by `build_mutex_`;
`build_condition_` wakes the worker for requests, cancellation, and shutdown.
Requests are moved under that mutex. Immutable `BuildCompletion` objects are
transferred back using Qt's queued event delivery. The GUI and CR-1 thread share
only atomic `ipc_stop_` and `ipc_client_socket_`. Socket paths, widgets, states,
and workspace data are not shared.

## Corpus states

| State | Meaning / transitions |
|---|---|
| Empty | Add -> Needs Build; Build is ignored; Reset stays Empty. |
| Needs Build | Build -> Building; Add stays Needs Build; Reset -> Empty. |
| Building | Success -> Current; failure -> Needs Build; Add -> Needs Build; Reset -> Empty; Build replaces the request; Exit -> Stopped. |
| Current | Build -> Building; Add -> Needs Build; Reset -> Empty. |
| Stopped | Worker joined; later events are ignored. |

State objects are created, invoked, and destroyed on the GUI thread. For
example, Add during a build first dispatches to
`BuildingCorpusState::workspace_will_change`, which increments the generation
and notifies the worker before the workspace mutates. After mutation,
`workspace_changed` clears old outputs and changes state. `wait_for_build_step`
throws `BuildCancelled`, so the old result is discarded;
`NeedsBuildCorpusState` can then submit the next snapshot.

## Decisions and evidence

1. Build during a build is allowed: `BuildingCorpusState::build_requested`
   submits a newer generation, replacing any pending request and cancelling
   the active one.
2. A remote add is queued to `add_remote_document` on the GUI thread. It
   follows the normal Add path, invalidates the corpus, and cancels the build.
3. The smallest build handoff is one mutex-protected pending request, generation,
   stopping/busy flags, and condition variable. The worker needs the generation
   to reject stale work, the pending snapshot to build, and the stop signal to
   exit; completion is immutable and queued back to the GUI.

`AIWS_FAULT_NO_CANCEL` is in `wait_for_build_step`; the reset-and-rebuild test
`reset_cancels_old_build_before_next_build` checks that the new build starts
promptly. `AIWS_FAULT_SILENT_SHUTDOWN` is in `stop_worker`;
`idle_shutdown_completes` checks shutdown in a child process.

## AI use

Copilot assisted with code changes and tests; I reviewed the design and behavior
against the specification.
