# AI_LOG.md

- Student Name: Manan Sharma
- Student ID: manansharma
- Project / Milestone: Milestone 4: Responsive AI Workspace and CR-1 Remote Add

## Part 1: Summary of AI Tools Used

- Tools Used: GitHub Copilot using Copilot SDK in VS Code.
- Scope of Assistance: GitHub Copilot provided limited help with parts of the M4
  specification review, implementation planning, debugging, and documentation.

## Part 2: Detailed Log of Substantial AI Assistance

### Component: Background Corpus Build

- What did the AI do? GitHub Copilot gave limited suggestions for a GUI-thread coordinator with
  a joinable `std::thread`, workspace snapshot and revision checking, a
  `std::future` completion result, build-delay handling, cancellation, and
  stale-result rejection.
- What did the AI get wrong / What was the flaw? The initial implementation
  required compilation corrections and its shutdown path needed care to ensure
  the worker was joined before owned state was destroyed.
- How did you fix/modify it? I reviewed and built the code in the course Docker
  image, corrected the thread ownership and result-completion code, and tested
  normal and faulted builds. The normal suite passes, and both fault builds
  fail their associated fault-detection tests.

### Component: CR-1 Remote Add

- What did the AI do? GitHub Copilot provided limited help with the POSIX Unix-domain
  socket server, the `aiws_send` client, the length-prefixed byte protocol,
  partial-read handling, GUI-thread document updates, command-line options,
  socket shutdown, and CMake integration.
- What did the AI get wrong / What was the flaw? The first server build had
  calls to `close()` that were shadowed by `MainWindow::close()`, so the Docker
  build failed. The implementation and documentation also needed cleanup to
  make the wire format and shared-state rules clear.
- How did you fix/modify it? I changed socket descriptor calls to `::close()`,
  rebuilt with CMake in the Docker course image, and verified:
  - unreadable and non-existent files return rejection exit code 1;
  - a reachable remote file returns exit code 0;
  - an unreachable server returns exit code 2; and
  - shutdown removes the socket file.

### Component: Project Documentation and History

- What did the AI do? GitHub Copilot gave limited help drafting parts of the M4 design checkpoint,
  decision record, CR-1 protocol documentation, pair-programming log, and
  repository history.
- What did the AI get wrong / What was the flaw? The pair-programming record
  initially needed the requested one-hour session and accurate halfway role
  swap. The first history rewrite also produced UTF-16 text instead of the
  required readable UTF-8 text.
- How did you fix/modify it? I reviewed and simplified the pair-programming
  log, changed the session to 2:15 PM-3:15 PM with a 2:45 PM role swap, and
  regenerated `HISTORY.txt` with UTF-8 encoding.

## Part 3: Student Verification Statement

I affirm that I have thoroughly reviewed all AI-assisted code listed above, that
I fully understand how it operates within the broader application architecture,
and that I am able to modify or debug it independently.
