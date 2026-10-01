# P5 design note — exec/launch, waitpid, minimal signals

Status: design for integrator review. Docs only; no code in this lane.
Milestone: browser.md §6 P5 (P5.1 exec/launch, P5.2 waitpid/status, P5.3 signals,
P5.4 PROC_T acceptance). Lane L2 (`browser/l2-p5-design`), base main@`07b6c00`.
Consumers: P5 implementer (kernel L2), L3 (sysroot `<signal.h>`, `environ`/`getenv`,
wrappers), L8 (launcher integration in the WebKit fork), L9 (PROC_T acceptance).

Grounding (all verified against this base tree unless noted):

- `src/kernel/process.c` (P1 thread model has landed: groups, `group_teardown`,
  `process_kill`, `process_waitpid`, `process_wake_all`), `src/kernel/program_loader.c`
  (`process_exec_current`, `load_and_run_program_in_scheduler_args`),
  `src/kernel/arch/{arm,x64}/trap.c` (SYS_EXEC / SYS_WAITPID / SYS_KILL / SYS_SPAWN),
  `src/kernel/fs.c` + `src/include/fs.h` (per-group `open_fds[32]`, `dup2`, fcntl flags),
  `src/kernel/pipe.c` (reader/writer counts; no SIGPIPE today),
  `src/include/{syscall.h,process.h,errno.h}`, `src/user_include/libc.h`,
  `src/libc/include/signal.h`, `src/libc/include/sys/wait.h`.
- WebKit fork clone at pinned tag `webkitgtk-2.54.0` (`~/webkit-hobbyos`): files cited
  inline. Vendored GLib 2.88.3 sources (`third_party/glib-2.88.3`, source copy
  verified in the L6 lane tree): files cited inline.
- Cross-notes: `third_party/glib-2.88.3/cross-notes.md` ("gspawn decisions"), and the
  GLib stance section of `browser.md` (~line 315).

Delta discipline (house rules carried from P1/P2): one surface per call; extend
existing rows in place where the ABI already matches; renumber before ship, never
after; any deviation from this note must be a recorded decision, not silent drift.

---

## 0. Constraints carried

- Kernel is C, no dynamic allocation, fixed tables: `MAX_PROCESSES 64` PCB slots,
  32 fd slots per process group, `proc_lock`-guarded state.
- No new *blocking* primitives that can deadlock a caller holding a lock; park-and-wake
  only (`PROC_STATE_BLOCKED` / `WAIT_CHILD` / `WAIT_SPAWN` / `WAIT_THREAD` established by P1).
- Threads share one process group; shared state (fd table, cwd, heap, argv blob) lives on
  the group anchor (`struct process` of the leader pid); per-thread state is context,
  fpu, wait queue, name, TLS.
- x64 trap-frame arg mapping: args are `regs[5]/[4]/[3]/[9]/[7]/[8]` (rdi/rsi/rdx/r10/r8/r9,
  per the x64 trap notes; P2 already documented r8 = arg5). ARM: `regs[0..]`.
- P2 (parallel lane) will introduce the sparse address space (`as`), `vm_touch`, and the
  fault classifier. P5 must not assume physical==virtual translation for user pointers
  after the P2 gate; all user-pointer access must route through the same helper P2 freezes.
- Evidence standard for the eventual implementation: unit tests both arches, PROC_T on
  both QEMU targets, no regressions in the existing batteries.

## 1. What exists today (verified)

**exec (row 40).** `SYS_EXEC` is real: path + argv array (2 args). In-place image
replacement via `process_exec_current`: same pid, same fd table, cwd kept; `argv` stored
in the caller's `eargv` blob (`HO_EXEC_MAX_ARGS 32`, `HO_EXEC_ARGV_LEN 256`, entries ≤ 63
chars, extras silently dropped). No envp. No CLOEXEC. Exec from a secondary thread is a
documented P1 divergence (siblings keep running the old image).

**waitpid (row 39).** Real: blocking and `WNOHANG`; `pid > 0` waits for that child leader,
`pid == 0 || pid == -1` waits for any child; status layout already POSIX-classic
(normal exit `(code & 0xff) << 8`; kills carry the signal number in the low 7 bits —
`process_kill` writes raw 9 today), decoded by the existing `sys/wait.h` macros. Blocking path parks the caller in
`PROC_STATE_WAIT_CHILD`; the result is delivered from `group_teardown` (and from the
thread-exit path) by rewriting the parked caller's saved syscall result. `ECHILD` when no
children. Known gap: **`process_kill`'s leader branch does not run reap delivery** — a
parent parked in `WAIT_CHILD` is not woken when its child is killed by a third party
(sysmon, desktop close). Confirmed by inspection this session (`process_kill` sets
`EXITED`, frees the block, closes fds, calls `process_wake_all()` — which only touches
`PROC_STATE_BLOCKED` — and does not deliver a status to a `WAIT_CHILD` parent).

**kill (row 16).** Exists; every signal except `sig == 0` is ignored ("no signals yet");
`sig == 0` is the existence check (`ESRCH` if absent) used by shell, desktop, tests.
`process_kill` implements P1 semantics: kill of a leader = whole group (all threads
`THREAD_DONE`, fd close, block free, zombie PCB kept if the parent is alive); kill of a
secondary thread = just that thread.

**spawn.** `SYS_SPAWN` + `spawn2`: kernel creates the child *directly* (no copy of the
parent): temporary spawn worker thread loads the image with `WAIT_SPAWN` + `spawn_retval`
word; caller parks until the worker reports pid or failure; on worker-creation failure
both arch dispatchers must release the caller with `-1` (fixed historical hang class —
keep this invariant). fd wiring today is the pipe/dup rule for slots 0/1/2 only.

**fd flags.** `dup2` exists (single-threaded callers today); fcntl supports only
`F_GETFL`/`F_SETFL` on sockets; no `F_GETFD`/`F_SETFD`/`F_DUPFD`; no `FD_CLOEXEC`
anywhere. `pipe_write` with no readers returns `-1` plus a `[PIPE_WRITE_ERR]` print —
no SIGPIPE, no `EPIPE`.

**Signals.** Sysroot `<signal.h>` has `sigset_t` + a placeholder `sigaction` that
records and returns success; libc `signal()` is a stub; `abort()` exits with the
signal-shaped status (`WTERMSIG == SIGABRT`). Fault deaths today: the arch trap dumps a
kernel line (`[KERNEL] User process fault! Vector: …`) and calls `process_exit`; the P2
design standardizes the classifier and the signal-shaped fault status (11). There is no
pending-signal state, no delivery, no dispositions in the kernel.

**Capacity.** 64 PCB slots; 32 MiB per-process region blocks, 40 blocks on ARM (232 on
x64, the roomier pool). A spawn transiently consumes caller + worker + child slots.

## 2. What WebKit/WPE actually needs (evidence)

### 2.1 The launcher path

`Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp` (2.54.0) launches every
child process with GLib:

- `g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_INHERIT_FDS)`, then
  `g_subprocess_launcher_take_fd (launcher, clientSocket, clientSocket)` to place the
  IPC socket fd at a fixed number in the child, argv `{executablePath, pidString,
  fdString}`, and the **parent's environment** (`GSubprocessLauncher` defaults to the
  current environ).
- The IPC socketpair is `SOCK_SEQPACKET` (`IPC::createPlatformConnection(SOCK_SEQPACKET, …)`
  in the launcher; `IPCUtilities.h` / `unix/IPCUtilitiesUnix.cpp` apply
  `SetCloexecOnClient|SetCloexecOnServer` via `fcntl(F_SETFD, FD_CLOEXEC)`) — i.e. the
  launcher manages CLOEXEC itself, then explicitly transfers the child side via the fd map.
- `terminateProcess` = `kill(pid, SIGKILL)` (SIGTERM first only under the PGO build,
  off in our config).
- The launcher **source comment states the spawn rule verbatim**: "we want GIO to be
  able to spawn with posix_spawn() rather than fork()/exec(), in order to better
  accommodate applications that use a huge amount of memory or address space in the UI
  process" — plus the three rules that keep GLib on the posix_spawn path: (a) must
  inherit fds, (b) must not search path from envp, (c) must not use a child setup
  function. This is the same economics D1 decides on for HobbyOS.
- Spawn failure is **fatal in the UI process**: the launcher `g_error()`s when
  `g_subprocess_launcher_spawnv` returns null (`g_error` aborts). `spawn_ex`'s failure
  paths must therefore be rare and correct, and `g_subprocess_get_identifier` (the child
  pid as a string) is the launcher's post-spawn assumption.

GLib's `gspawn-posix.c` spawn core (both paths, read this session):

- **posix_spawn fast path** is preferred whenever `HAVE_POSIX_SPAWN` is set ("posix_spawn()
  is assumed the fastest way to spawn"); it uses `posix_spawnattr_setflags` with
  `POSIX_SPAWN_SETSIGDEF` resetting **SIGCHLD, SIGINT, SIGTERM, SIGHUP**, and
  `posix_spawn_file_actions_adddup2` for the fd map (source == target entries rely on
  dup2's clear-CLOEXEC rule).
- **fork fallback** (what runs if the port's libc has no `posix_spawn`): `fork()`, then in
  the child: reset the same five dispositions via legacy `signal()` (**including
  `signal(SIGPIPE, SIG_DFL)`**), close the report pipes, apply the fd map, `fdwalk` the
  child's CLOEXEC set, `execve`, and report failures over an err-pipe.

Child side, `Source/WebKit/Shared/unix/AuxiliaryProcessMain.cpp`: parses
`{processIdentifier, connectionIdentifier}` from argv, and **`sigaction(SIGPIPE, SIG_IGN)`
with a RELEASE_ASSERT on success** before anything else. This is the child's only
signal setup; it is load-bearing: without a working SIGPIPE-ignore the child aborts at
startup, and without *kernel* SIGPIPE semantics an `EPIPE` write kills it.

Exit detection: the UI process learns of a child's death from the IPC connection close
(`ProcessLauncher.cpp` / `WebProcessProxy` paths; no waitpid in WebKit code). GLib reaps:
`gio/gsubprocess.c` registers a `g_child_watch_source_new(pid)` immediately after spawn;
`glib/gmain.c`'s child-watch machinery installs a **SIGCHLD handler** (`sigaction` with
`SA_RESTART | SA_SIGINFO`; the registered function takes one argument) and a worker
thread loops `waitpid(pid, WNOHANG)`, woken by a pipe write from that handler. So in the
GLib launcher route, **handler delivery for SIGCHLD must actually run**, not just be
accepted.

`third_party/glib-2.88.3/cross-notes.md` ("gspawn decisions") records the earlier framing
(launcher uses `g_spawn_async`; the `/proc`-based fd cleanup in gspawn is flagged as the
stubbed area to audit for a no-/proc target) — the pinned 2.54.0 code reaches the same
gspawn core through GSubprocess (§2.1). Its conclusion stands and P5 agrees: gspawn/
posix_spawn is a hard requirement for the WPE multi-process model — if HobbyOS can't
provide it, the launcher becomes a port-specific fork (Route B, D12).

### 2.2 WTF / JSC signal usage in our configuration

- `Source/WTF/wtf/posix/ThreadingPOSIX.cpp`: `WTF::initialize()` **RELEASE_ASSERTs** that
  `sigaction(SIGUSR1, …)` succeeds (attemptToSetSignal); suspend/resume is
  `pthread_kill(target, SIGUSR1)` + the handler self-parking on a semaphore via
  `sigsuspend`. `MachineStackMarker::gatherFromOtherThreads` (GC other-thread stack scan)
  uses `Thread::suspend()` → this machinery whenever **two or more threads have entered
  the JS VM** (worker threads, `JSLock` holders). Single-threaded JS never reaches it.
- `ENABLE_SIGNAL_BASED_VM_TRAPS` is defined 1 (`PlatformEnable.h:958`) but is gated on
  DFG JIT, which our JIT-off configuration does not enable; Wasm off. JSC's VMTraps
  signal handlers are therefore **not installed** in our build (`jsc` shell opts in
  explicitly; the browser does not).
- Crash-reporter signal handlers (Breakpad/Crashpad) are off.
- Conclusion: the *startup* asserts require `sigaction(SIGUSR1)` and
  `sigaction(SIGPIPE, SIG_IGN)` to succeed; the only browser path that *executes* a
  handler is GLib's SIGCHLD (route A). SIGUSR1's suspend/resume execution path is the
  known landmine (D13/OQ3).

### 2.3 Requirement list distilled

1. `execve` with argv + envp + fd/CLOEXEC rules (shell, gspawn fork fallback, exec-in-child).
2. A direct spawn that applies an fd map, envp, argv, and CLOEXEC semantics **without a
   fork copy of the UI process** (fork of the UI process copies tens of MiB under P2 —
   unacceptable per launch).
3. `waitpid` pid-specific + WNOHANG (GLib worker loop), blocking + accurate statuses
   (HobbyOS-side launcher reap), and correct reap wakeups for third-party kills.
4. `kill(SIGKILL|SIGTERM)` for the launcher's terminate path; `kill(pid,0)` unchanged.
5. SIGPIPE: ignore disposition, `EPIPE` returns, fatal default.
6. SIGCHLD: pending → run handler (route A) or at least a reliable reap signal (route B).
7. `sigaction` success surface for the WTF startup asserts.
8. Environment readback (`getenv`) — WebKit reads env-based config (e.g. exec-path
   overrides) at startup.
9. fork + exec must remain correct as the fallback combination (gspawn fork path).

## 3. Decisions

### D1 — Launch model: in-place `execve` + a direct spawn syscall; fork+exec is a fallback, not the launcher path

**(a)+(c) combined, (b) kept as internal basis.** `execve` (D2) is the POSIX in-place
primitive. The launcher path gets `SYS_SPAWN_EX` (D4): the kernel creates the child
directly from the image (the existing spawn2 worker machinery generalized), applying
argv/envp/fd-map/CLOEXEC at creation. Existing `spawn`/`spawn2` stay for current users.

Rationale:

- The direct spawn is the only option whose cost is independent of the parent's address
  space. Under P2, `fork()` of the UI process copies its resident pages; a WebProcess is
  spawned on first navigation and more on crashes — a 30–80 MiB copy per launch is the
  wrong default (measured cost is a WK-2 milestone question, but the design must not
  bake it in). WebKit's own launcher comment says the quiet part out loud: posix_spawn
  exists "to better accommodate applications that use a huge amount of memory or address
  space in the UI process" (§2.1).
- GLib's own fast path is posix_spawn-style (`vfork`/clone semantics in glibc), i.e. not
  a plain fork+exec — matching the direct spawn is *closer* to what runs on Linux than
  fork+exec is.
- Rejected: **fork+exec as the launcher path** — copy cost, plus fork of a
  multi-threaded UI process is the hardest case for our P1 thread model (fd and memory
  copy under threads); keep it working (gspawn fallback) but don't depend on it.
- Rejected: **extending `SYS_SPAWN`/`spawn2` in place** — its ABI (5 args, fd slots 0/1/2
  pipe rule) is a different shape than the launcher needs and is used by shell/tests;
  changing it is the duplicate-surface trap in reverse. `SPAWN_EX` is additive; the
  spawn2 *implementation* pattern (worker + `WAIT_SPAWN` + release-on-failure) is reused.
- Rejected: **a `posix_spawn`-only design** (implement glib's call literally in libc) —
  the libc adapter is still wanted (OQ2) but it must sit on a kernel primitive; writing
  `posix_spawn` semantics in user space over fork would reintroduce the copy.

### D2 — Syscall rows: extend 40/39/16 in place; new rows 83–86

| row | old | P5 shape | note |
|---|---|---|---|
| `SYS_EXEC` | 40 (2-arg) | 40, **(path, argv\*, envp\*)** | extended in place; `execve` |
| `SYS_WAITPID` | 39 | 39, semantics completed | options validation, D5 |
| `SYS_KILL` | 16 | 16, semantics completed | signals, D9 |
| `SYS_SIGACTION` | — | **83** `(signum, act\*, oldact\*) -> 0 \| -errno` | new |
| `SYS_SIGRETURN` | — | **84** `(void) -> resumes` | new; called by libc trampoline only |
| `SYS_GETENV` | — | **85** `(idx, buf, size) -> len \| -errno` | new; mirror of GETARGV (64) |
| `SYS_SPAWN_EX` | — | **86** `(path, argv\*, envp\*, fdmap\*, n) -> pid \| -errno` | new; D4 |

Amendment to browser.md §A.1b (record this in the doc at the next edit): the provisional
`EXECVE 83` / `WAITPID 84` / `KILL 86` rows are **withdrawn** in favor of in-place
extensions of the rows that already exist (40/39/16 — P2's MMAP-62 precedent); the freed
numbers carry P5's actually-new rows, preserving the "contiguous from 72" intent:
76–79 P4, 80–82 P2, 83–86 P5. `SYS_MAX` becomes **86** at the P5 gate (single-writer
edit). If P4's frozen gate moved its range, renumber the P5 block before ship (P1
precedent: renumber, never duplicate).

x64 arg mapping note for implementers: 3-arg rows use `regs[5]/[4]/[3]` (rdi/rsi/rdx);
5-arg rows use `regs[5]/[4]/[3]/[9]/[7]` (rdi/rsi/rdx/r10/r8) — same as P2's five-arg
convention. ARM: `regs[0..4]`.

### D3 — `execve` exact semantics (row 40)

1. `argv` (unchanged caps: 32 entries, 256-byte blob, 63-char entries; overflow is
   truncated as today — documented, not an error, because the launcher passes 3 entries
   and existing callers rely on it). `envp`: new blob on the group anchor
   (`HO_ENV_LEN 512`, NUL-separated, count kept). **`envp == NULL` means empty
   environment** (documented divergence-from-glibc corner; `execv` passes the caller's
   `environ`, so no real caller hits it).
2. **CLOEXEC sweep at exec**: every fd with `FD_CLOEXEC` set in the group's
   `fd_cloexec` mask is closed (file_close, slot freed) before the new image runs.
   Non-CLOEXEC fds survive exec **with their fd numbers** (0/1/2 and the launcher's
   transferred IPC fd are exactly this case). `dup2` (and `dup`) **clear the CLOEXEC bit
   on the new fd** (POSIX; glib's source==target fd-map trick relies on the dup2 rule).
3. cwd kept; pid/fd-table/heap reset/argv replaced as today; **signal state reset**:
   handled dispositions → `SIG_DFL`, ignored stay ignored, pending cleared, delivery
   state cleared, new env applied.
4. **From a secondary thread**: POSIX semantics — all other group threads are terminated
   first (reuse the P1 `exit_group` sequence: mark `THREAD_DONE`, drain claims, then
   replace the image; the calling thread survives as the new program). This removes the
   documented P1 divergence; the exec'ing thread keeps the anchor's pid.
5. Failure: no state change; `-ENOENT` (no image), `-ENOEXEC` (bad image), `-EFAULT`
   (bad user pointers), `-ENOMEM`. Silent-truncation of argv stays (see 1).
6. P2 interplay: the new image load goes through the same loader path as today; the
   loader's region setup is P2's business (dual-AS promotion). The argv/env blobs are
   kernel-side, unaffected.

Rejected: a separate `SYS_EXECVE` (duplicate surface; P2 MMAP-62 precedent); making
`envp` a first-class `environ`-pointer ABI (kernel copies strings once — bounded; keeping
the blob is what `GETARGV` already does).

### D4 — `SYS_SPAWN_EX` exact semantics (row 86)

`spawn_ex(path, argv, envp, fdmap, n)` — create a child from `path` with:

1. **fd inheritance = copy of the parent's fd table** (like fork: `fs_reopen` per open
   slot — this is what INHERIT_FDS means for WebKit), then **apply `fdmap`**: `n` pairs
   `{src, dst}` behave like `dup2(src, dst)` in the child (dst's CLOEXEC cleared; illegal
   dst → `-EBADF`, whole spawn fails atomically), then **apply the CLOEXEC sweep** (close
   remaining CLOEXEC fds). Order matters and matches gspawn: map first, sweep second.
2. **Dispositions in the fresh child are all default** — this satisfies glib's
   `POSIX_SPAWN_SETSIGDEF(SIGCHLD, SIGINT, SIGTERM, SIGHUP)` by construction; no extra
   flag needed. (The fork+exec fallback reproduces it via fork-inherit + exec-reset, D3.3.)
3. Parent: the caller's group anchor is the parent (waitpid/wait-for-child semantics
   unchanged, P1 rules apply). Child pid returned to the caller.
4. Implementation: **reuse the spawn2 worker machinery** (kernel worker thread + caller
   parks in `WAIT_SPAWN` + `spawn_retval` word + retry loop + **release-on-worker-failure
   in both arch dispatchers** — the historical hang class must not reappear; the worker
   carries the spawn_ex payload, including the fdmap copy). Return `pid` on success,
   `-errno` on failure (`-ENOENT`, `-ENOEXEC`, `-EFAULT`, `-EINVAL` bad fdmap,
   `-EAGAIN` slot pressure, `-ENOMEM`).
5. cwd inherited from the caller (spawn2 behavior); argv[0] stored as given (the child's
   `argv[0]` is the path the launcher passed — WebKit's aux main parses argv[1..], so no
   extra rule is needed); envp as D3.1.
6. libc (L3/P7): `spawn_ex()` wrapper now; a `posix_spawn`/`posix_spawn_file_actions_*`
   adapter over row 86 is **designated for L3** with this kernel surface frozen here
   (OQ2). The glib fork fallback needs no adapter (fork + exec + fdwalk).

Rejected: passing a close-list (glib's GSubprocess path never closes fds; CLOEXEC covers
it); making spawn_ex async with a pid-ready event (the caller park is short — worker does
disk I/O — and mirrors existing machinery).

### D5 — `waitpid` completion, reap unification, zombie/slot pressure

1. Options: accept `WNOHANG` (1) and `WUNTRACED` (accepted, ignored — no stop states);
   any other bit → `-EINVAL` (today silently ignored; tightening is safe, no in-tree
   caller passes more). `pid > 0` = the child leader; `pid == 0 || pid == -1` = any child;
   `pid < -1` = any child (documented divergence: no pgid sets beyond a group's own pid;
   WebKit never uses it).
2. Status layout is **frozen as-is** (tests depend on it): normal exit status in bits
   8–15 (`(code & 0xff) << 8`); signal deaths carry the signal number in the low 7 bits
   (`process_kill` writes raw 9 today; the P2 fault path writes 11; 13/15/6 follow from
   D9/D10) — the `WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG` subset the sysroot macros
   already decode.
3. **Unify reap delivery**: extract the delivery block currently in `group_teardown`
   ("wake a parked `WAIT_CHILD` parent, write the status, free/keep zombie per parent
   liveness") into one helper, and call it from **all child-death paths**:
   `group_teardown` (thread exits, exit_group, natural death) and `process_kill`'s leader
   branch (fixes the confirmed third-party-kill gap). The exec-from-thread sequence
   (D3.4) terminates sibling *threads* only — it must not be wired through the helper.
4. **SIGCHLD pending** is set on the parent group by the same helper (D8.3), so route A
   (GLib) and route B (own reaper) both observe death promptly.
5. Zombie policy (unchanged, restated): an `EXITED` PCB with a live parent is preserved
   for reap; reclaim to `FREE` happens only on reap, on parent death (P1 orphan path), or
   when no parent group exists. **No new blocking**: reap delivery never waits on
   anything — it is O(slots) under `proc_lock`, list walk, status word write, wake.
6. Slot pressure (64 PCBs): budget for the launcher model —
   UI + Network + WebContent = 3 persistent; + spawn worker transient (≤2: caller's
   worker + the load) = ~5; + desktop shell/utility = 6–7 typical. Gate P5 requires a
   measured ≥6 concurrent live processes plus a **30× spawn/exit stress loop with zero
   slot/fd/block leak** and a leak counter in the existing `[BLOCKS]`/process-slot
   forensic. Extend the no-slot forensic line to count `EXITED`-with-live-parent slots so
   "UI process stopped reaping" is diagnosable on device.
7. P2 interplay: the status write and the parked-caller saved-result rewrite must use
   P2's user-pointer helper once it lands; until then the current translation is fine.
   Flagged for the implementer (OQ5).

Rejected: making all waitpid calls WNOHANG-only for the launcher (route B could, but the
shell/tests need blocking waits and the helper unification is the same work); adding a
separate `SYS_WAITID`/`siginfo`-returning wait (deferred with siginfo fidelity, D11).

### D6 — Signal set (frozen, minimal)

| sig | no | default | catch/ignore | sendable via kill | delivered to handler | source |
|---|---|---|---|---|---|---|
| SIGHUP | 1 | terminate | yes | yes | yes | terminal-ish, glib reset list |
| SIGINT | 2 | terminate | yes | yes | yes | glib reset list |
| SIGABRT | 6 | terminate + report | recorded, **not delivered** | yes | no | `abort()`/raise (declared faults) |
| SIGBUS | 7 | terminate + report | recorded, not delivered | yes | no | P2 fault classifier |
| SIGKILL | 9 | terminate (**uncatchable**) | `EINVAL` on sigaction | yes | n/a | launcher terminate |
| SIGUSR1 | 10 | terminate | accepted, **delivery deferred** | yes | no (D13) | WTF suspend (deferred) |
| SIGUSR2 | 12 | terminate | accepted, delivery deferred | yes | no | WTF `Signal::Usr` |
| SIGPIPE | 13 | terminate | yes (`SIG_IGN` is the WebKit case) | yes | yes | pipe/socket writes, D10 |
| SIGTERM | 15 | terminate | yes | yes | yes | launcher (PGO path), users |
| SIGCHLD | 17 | **ignore** | yes (GLib child watch) | yes | **yes (the only must-run)** | child death, D5.4 |
| SIGSEGV | 11 | terminate + report | recorded, not delivered | yes | no | P2 fault classifier |
| SIGILL | 4 | terminate + report | recorded, not delivered | yes | no | kernel fault path (P2 design) |

- Numbers ≤ 31 (`HO_SIG_MAX 32` for the tables). `sigaction` on any other number →
  `-EINVAL`; `kill` with an unknown signal → `-EINVAL`.
- **Accept-and-record** (not reject) for the fault-class and SIGUSR1/USR2 dispositions:
  the placeholder sysroot already accepts, ported programs (nano installs SEGV/ABRT
  handlers) must not start failing, and the WTF startup asserts must pass. The divergence
  (handler recorded but never invoked for fault-class signals; the default action — kill +
  report — still happens) is documented here and in the sysroot header.
- SIGKILL/SIGSTOP are the only `EINVAL`-rejected `sigaction` targets (POSIX).
- SIGUSR1 delivery is deliberately *not* enabled even tentatively: a mis-delivered
  suspend/resume signal could let WTF's `suspend()` proceed on a thread that is not
  actually suspended (GC stack-scan corruption class). Pending bits for it accumulate
  harmlessly; D13/OQ3 owns the follow-up.

### D7 — Disposition and pending storage

Per group anchor (appended to `struct process`; threads share it; leaders own it):

```c
/* P5: exec/launch + signals (see docs/browser/p5-exec-signals-design.md) */
char    env[HO_ENV_LEN];          /* NUL-separated entries, "" = empty set */
int     envc;                     /* entry count */
uint32_t fd_cloexec;              /* FD_CLOEXEC bitmask, bit i = fd i */
uint32_t sig_pending;             /* pending bits, bit (sig-1) */
uint64_t sig_handler[HO_SIG_MAX]; /* 0 = SIG_DFL, 1 = SIG_IGN, else user VA */
uint64_t sig_restorer;            /* sigreturn trampoline VA (one per group) */
int      sig_in_handler;          /* 1 while a handler frame is active */
uint64_t sig_frame;               /* active signal frame VA (valid when in_handler) */
```

- `sigaction(sig, act, oldact)`: validates `signum`, copies dispositions both ways
  (`oldact` query path must succeed for WTF: accept `act == NULL`), requires a valid
  in-region `sa_restorer` when installing a real handler (libc fills it by default —
  D8.2), stores. All return `0` for the supported set — the WTF/child asserts pass.
- No per-thread masks in P5: `sigprocmask`/`pthread_sigmask` are libc-level no-ops
  returning `0` (documented; the only GLib use — `gmain.c` line ~6814, unrelated to the
  child-watch path — tolerates it; verified this session). `sigsuspend` → `-ENOSYS`
  (must not be a no-op: a no-op would silently break WTF's suspend handler semantics).
- NSIG storage cost: ~280 B added to the anchor PCB; fine within `MAX_PROCESSES 64`.

### D8 — Delivery engine (minimal, main-thread)

1. **Delivery thread**: the group leader only (plan's "main-thread delivery"; matches
   GLib's SIGCHLD handler being installable anywhere and run by any thread — running it
   on the leader is sufficient). If the leader has exited while siblings live, pending
   signals are retained but not delivered; default actions still apply. Documented edge;
   WebKit's launcher keeps its main thread for life.
2. **Frame + trampoline**: delivery builds a signal frame on the target's user stack
   (16-byte aligned, immediately below SP) saving the full register context + FP/SIMD
   state + a minimal `siginfo` (`si_signo`, `si_code`, `si_pid`, `si_status` where known;
   other fields zero — GLib reads only the signal number from its one-argument handler).
   The interrupted PC/SP/flags are in the frame; the **return address** for the handler is
   `sig_restorer` (Linux `SA_RESTORER` model): the kernel stores it from the `sigaction`
   struct; libc's wrapper fills `sa_restorer` with `__ho_sigreturn_trampoline` when the
   caller left it 0. The trampoline is two instructions in libc `.text`
   (`mov x8,#84; svc #0` / `mov eax,84; syscall`) — no kernel-emitted executable code, no
   VDSO, and no conflict with P2's non-executable stacks. Handler entry follows each
   arch's ABI call state (ARM: `x0 = signum`, `x30 = restorer`; x64: RSP ≡ 8 mod 16 with
   the restorer at `[RSP]` — the historical user-entry alignment rule applies here too).
3. **SIGCHLD generation**: parent anchor gets the pending bit (D5.4); if the leader is in
   a restartable park (select/poll/sleep slice), wake delivery at the next slice restart;
   otherwise delivery happens at the next return-to-user boundary.
4. **Delivery points**: (a) every trap exit to user mode of the leader (syscall return,
   timer-preempt resume); (b) select/poll/sleep park restarts (abandon the park without
   consuming the deadline, plant the handler frame; `SIGRETURN` resumes the parked
   syscall — the same rewind technique as the existing `-2` restart). **Blocked
   `pipe_read`/`pipe_write`/`WAIT_CHILD`/`WAIT_SPAWN`/futex parks: delivery deferred
   until the syscall completes** (documented; no EINTR, no restart-hook generalisation in
   P5). Fatal signals (default action) do not need delivery: the kill takes effect
   immediately, even mid-park.
5. **No nesting**: while `sig_in_handler` is set, further handler delivery is deferred
   (pending accumulates, delivered after `SIGRETURN`). SIGCHLD re-entrancy from within a
   handler is not needed by GLib's (async-signal-safe, flag + pipe write) handler;
   documented divergence.
6. **SIGRETURN** (row 84): restores the full frame (registers + FP + interrupted
   PC/SP/flags), clears `sig_in_handler`, re-checks pending delivery, returns to the
   interrupted context. Called with no arguments; a stale/no-frame call → `-EINVAL`,
   process continues (defensive). Exec clears it (D3.3).
7. **FP state**: saved into the frame at delivery and restored at `SIGRETURN` using the
   existing per-arch fpu save/restore code refactored into destination-pointer variants
   (`arch_fpu_save_to(void*)` / `arch_fpu_restore_from(void*)`); this keeps handlers that
   use floating point from corrupting the interrupted context. Thin arch change, flagged
   for the FP owners (OQ6).
8. Stack bounds: if the leader's SP leaves no room in the mapped stack (guard hit), force
   the signal's default action (kill + report) rather than faulting in delivery.

Rejected: per-thread directed delivery + `sigsuspend` + machine-register export (that is
the WTF suspend interface — D13, needs a bigger design); kernel-emitted stack trampolines
(no-exec stacks under P2); skipping handler execution entirely (route A would break).

### D9 — `kill` semantics (row 16 completion)

1. Target resolution: **signals are process-directed.** `pid > 0` names any group member
   (leader or thread) → the signal targets that member's *group*; `pid == 0` → caller's
   group; `pid < 0` → group leader `-pid`. No match → `-ESRCH`. `sig == 0` remains the
   existence check (unchanged).
2. Per disposition: `SIG_IGN` → no-op, returns 0. Handler → set pending (D8). Default →
   immediate group kill with `WTERMSIG = sig` (status records the signal byte); SIGKILL
   is uncatchable/ignore-proof and always the immediate kill path (existing
   `process_kill`). Group kill keeps P1 semantics (no blocking on claims; accepted class).
3. Change from P1: the secondary-thread-only kill branch loses its public meaning —
   kill(thread-tid) now signals its whole group. **No in-tree caller relies on
   thread-only kill** (verified: sysmon/desktop/shell use process pids), so this is a
   semantics fix recorded here (OQ7 for the integrator to ratify).
4. The kill path also runs the unified reap delivery (D5.3) so a parent blocked in
   `WAIT_CHILD` learns of the death — the confirmed gap this design closes.

### D10 — SIGPIPE and `EPIPE`

1. One kernel helper `signal_epipe(group)` used by (a) `pipe_write` when
   `reader_count == 0`, (b) socket writes on a closed/reset peer (P4 socketpair/net path —
   audit at implementation; pipe is the P5 gate case).
2. Behaviour: `SIG_IGN` (the WebKit child's disposition) → return `-EPIPE`, writer
   survives; handler → set pending **and** return `-EPIPE`; default → terminate the
   writer's group with `WTERMSIG = SIGPIPE` (13). Keep the existing diagnostic print but
   demote it to a rate-limited `[PIPE_WRITE_EPIPE]` (it currently fires once per write and
   is noisy for the ignore case).
3. `errno.h` already has `EPIPE 32`; libc passes it through.

### D11 — Crash and exit reporting (closes browser.md §10 OQ6 as designed here)

1. Fatal signal death prints one kernel line, bounded, existing format extended:
   `[KERNEL] Process N (NAME): died on signal 11 (fault VA=...)` for faults (the P2 fault
   line stays; P5 keeps the signal number consistent with the status), and for kills
   `[KERNEL] Process N (NAME): killed by signal 15 (ptid P)`. Rate-limit to one line per
   death (exists).
2. Parent contract (what the UI process / shell can rely on):
   `waitpid` returns a status with the signal byte set (`WIFSIGNALED` true,
   `WTERMSIG = 11/6/7/4/9/13/15`); GLib's child watch or the port reaper maps it to
   WebKit's termination callback. No siginfo/backtrace channel in P5 (out of scope; the
   kernel line + exit status are the crash report).
3. `abort()` in libc on device becomes `raise(SIGABRT)` → default kill + report (updates
   the current "no signals yet" exit path; same status byte, now with a real kill).

### D12 — Launcher integration routes (recommendation; L8 decision point)

- **Route A (glib-as-is)**: the fork keeps `ProcessLauncherGLib`; the OS must satisfy
  GSubprocess/posix_spawn. Consumes from P5: `execve` 3-arg, `spawn_ex`, CLOEXEC fd
  semantics, `waitpid(pid, WNOHANG)`, SIGCHLD handler delivery, SIGKILL/SIGTERM,
  SIGPIPE-ignore, env; plus libc `posix_spawn` adapter + `F_DUPFD`/`F_DUPFD_CLOEXEC`
  (glib's `dupfd_cloexec`).
- **Route B (port-specific launcher)**: `ProcessLauncherGLib.cpp` gets an
  `#if OS(HOBBYOS)` branch calling `spawn_ex` (+ socketpair/fd map equivalents) and the
  port provides its own child reaper (waitpid WNOHANG on the IPC-close path). Fewer glib
  internals to satisfy (no gspawn, no posix_spawn, no SIGCHLD handler if the reaper is
  threaded or polling). This is the `cross-notes.md` fallback route.
- **Recommendation: Route B as primary** (matches the fork's existing platform-branch
  pattern at L8; less OS surface to gamble on), **Route A kept viable** because it is the
  cheaper fix if the fork-patch proves invasive, and because P5's SIGCHLD/exec work makes
  it reachable. The route choice is an L8/WK-2 gate item (OQ2); P5 builds the union of
  both routes' OS needs (the list above minus the posix_spawn adapter, which is L3).

### D13 — Explicitly deferred (with triggers)

| deferred | why safe now | revisit trigger |
|---|---|---|
| SIGUSR1/SIGUSR2 per-thread delivery, `sigsuspend`, machine-register export (WTF suspend/resume) | single-JS-thread config; nothing executes these handlers in the v1 browser path; startup asserts don't need delivery | first multi-JS-thread scenario (workers + GC stack scan), WK-3; options: (i) P5.5 per-thread delivery extension, (ii) fork-patch `ThreadingHOBBYOS`/`MachineThreads` to a cooperative variant |
| signal masks, `sigprocmask`/`pthread_sigmask` (no-ops), nesting/alt-stacks, SIGSTOP/SIGCONT/job control | no browser path uses them; shell jobs are pid-based | any ported component that masks (audit at L8 build date) |
| siginfo fidelity, `SA_SIGINFO` ucontext contents, core dumps, realtime signals | GLib reads only signum; WebKit crash-report handlers are off | crash-reporter enablement (post-M1) |
| VMTraps/Wasm signal handlers | JIT-off: handlers not installed (`ENABLE_SIGNAL_BASED_VM_TRAPS` requires DFG) | enabling DFG/JIT |
| `F_DUPFD`/`F_DUPFD_CLOEXEC` beyond D4/fcntl set | Route B doesn't need them; Route A does — implement with the posix_spawn adapter if Route A wins | Route A gate (WK-2) |

## 4. Frozen rows, structs, and supporting surface (implementation checklist)

Rows (final for P5 gate): **40** `execve(path, argv, envp)` · **39** `waitpid` (options
validated) · **16** `kill` (signal semantics) · **83** `sigaction` · **84** `sigreturn` ·
**85** `getenv` · **86** `spawn_ex` · `SYS_MAX 86`. Existing `SYS_SPAWN`/`spawn2`,
`SYS_FORK`, `SYS_DUP`, `SYS_PIPE`, `SYS_SOCKETPAIR` unchanged (spawn2's fd-slot pipe rule
stays for its users).

Struct additions: D7 block appended to `struct process` **after P2's `as` field**
(merge order: P1 → P2 → P5); nothing removed, nothing moved.

Sysroot/libc (L3, listed for coordination): `<signal.h>` adopts the Linux-compatible
`sigaction` layout — `union { sa_handler; sa_sigaction; }`, `sa_mask`, `sa_flags`,
`sa_restorer` — plus a minimal `siginfo_t`; libc `sigaction` fills `sa_restorer` and
supports the query form; `signal()` sets dispositions and returns the old handler;
`raise()` = `kill(getpid(), sig)`; `getenv()`/`environ` built from `getenv(85)` at crt0
time (exec-transparent); `spawn_ex()` wrapper; `posix_spawn` adapter deferred to L3
(OQ2); `waitpid` macros unchanged; `kill` unchanged.

Diagnostics: one line per noticed death (D11); `[PIPE_WRITE_EPIPE]` demoted; no-slot
forensic counts live-parent zombies (D5.6).

## 5. Test plan

**Host-testable (L3/sysroot parity):** `sigaction` wrapper (restorer auto-fill, query
form), `signal()` old-handler return, wait-status macro decoding, `getenv`/`environ`
scanning, `spawn_ex` argument marshalling — pure libc, existing host test harness.

**Kernel unit tests (both arches, `unit_test.c` tier):** signal table validation
(`sigaction` on SIGKILL → `EINVAL`, unknown → `EINVAL`, query form, restorer
requirement); disposition/pending state machine (ignore vs default vs handler; pending
set/clear; no-nesting deferral); `kill` targeting + `-ESRCH`; status-byte layout for
signals 9/13/15; `errno` paths for rows 16/39/40/83/84/85/86; check `spawn_ex` arg/fdmap
validation on fake PCBs (no disk) — keep disk-touching loads out of the unit tier.

**PROC_T.BIN acceptance (`src/user/proc_test.c` extension; runs on both QEMU targets):**

1. *P5.4 core (existing plan)*: socketpair talk; spawn; exec; exit code; reap.
2. *SIGCHLD observed*: parent installs a SIGCHLD handler (sets a flag, writes a pipe);
   child exits; parent's flag is set within a bounded loop and `waitpid(WNOHANG)` reaps
   with the right status — this is the GLib child-watch pattern end to end.
3. *SIGPIPE three ways*: SIG_IGN writer → `-1/EPIPE`, survives; default writer → dies,
   parent reaps `WTERMSIG 13`; handler writer → handler runs, then `EPIPE`.
4. *SIGTERM vs SIGKILL*: child ignores SIGTERM → `kill(TERM)` no-op, still alive;
   `kill(KILL)` kills; `WTERMSIG 9` reaped. Third-party kill while parent **blocked in
   waitpid** — parent wakes with the status (the D5.3 fix).
5. *exec/env/CLOEXEC*: `execve` with envp → child `getenv`s it; argv round-trip;
   `F_SETFD(FD_CLOEXEC)` fd disappears across exec (child probes `EBADF`), non-CLOEXEC fd
   survives; `spawn_ex` fd map delivers a socketpair fd at a chosen number.
6. *Exec-from-thread*: thread execs; siblings gone; anchor pid preserved; shell-visible
   behavior sane.
7. *Capacity/leak*: ≥6 concurrent live processes; 30× spawn/exit loop; slot/fd/block
   counters flat.
8. *WTF-init preconditions*: `sigaction(SIGUSR1,…)` and `sigaction(SIGPIPE, SIG_IGN)`
   both succeed (the two RELEASE_ASSERTs), and a subsequent `kill(SIGUSR1)` does **not**
   mis-deliver (no handler run) — pins the D13 boundary deliberately.

**Gate P5 (from browser.md, plus this note's additions):** PROC_T green on ARM + x64;
no regressions (`host_tests`, unit both arches, full wave suites, xcalc E2E); the
capacity/leak evidence above recorded in §11; the A.1b row amendment applied at the gate.

**Sequencing for the implementer (each stage gate-green before the next):**
S1 rows 83–85 storage/validation + `kill` semantics + SIGPIPE/EPIPE (no frames yet) →
S2 reap unification + waitpid completion + SIGCHLD pending (route B viable here) →
S3 frame engine + SIGCHLD delivery (route A viable) → S4 `execve` completion (envp,
CLOEXEC, from-thread) → S5 `spawn_ex` + libc `spawn_ex` → S6 PROC_T/batteries.

## 6. Decisions & open questions

**Decisions (this note):**

- D1 launch = in-place `execve` + direct `spawn_ex`; fork+exec fallback only.
- D2 rows: extend 40/39/16 in place; new 83 `sigaction`, 84 `sigreturn`, 85 `getenv`,
  86 `spawn_ex`; `SYS_MAX 86`; A.1b provisional rows withdrawn.
- D3 `execve` semantics: 3-arg, env blob, CLOEXEC sweep + dup2-clears-CLOEXEC, cwd kept,
  POSIX from-thread exit_group, dispositions reset, failure errnos.
- D4 `spawn_ex` semantics: fd-table copy + fd map + CLOEXEC sweep, fresh dispositions,
  spawn2 worker machinery with release-on-failure, errno set.
- D5 waitpid: option validation, frozen status layout, unified reap delivery (fixes
  third-party-kill wakeup gap), zombie policy, slot budget + leak gate.
- D6 signal set table (frozen); accept-and-record divergence for fault-class + USR1/2.
- D7 anchor struct: dispositions/pending/restorer/delivery state; masks are no-ops.
- D8 delivery: leader-only, frame + `sa_restorer` trampoline, no nesting, FP saved,
  delivery points incl. select-slice wakes, blocked parks defer, SIGRETURN row.
- D9 `kill`: process-directed targeting, disposition-based action, uncatchable SIGKILL.
- D10 SIGPIPE/`EPIPE` helper + three-way behaviour.
- D11 crash/exit reporting: kernel line + status byte contract (closes §10 OQ6).
- D12 launcher routes A/B; recommend B primary, A viable; union built in P5.
- D13 deferred list with revisit triggers.

**Open questions (genuinely unresolved — prefer decisions, these need other owners):**

- OQ1 **A.1b amendment consent** (rows 83–86 as above; 40/39/16 in-place extensions;
  `SYS_MAX 86`). Fallback if the integrator prefers minimal churn: keep 85/86 for
  `sigaction`/`sigreturn` and leave 83/84 as provisions — do not duplicate rows.
- OQ2 **Launcher route A vs B** (L8, at WK-2) and who owns the libc `posix_spawn`
  adapter (L3 vs L8) if Route A wins; also `F_DUPFD(_CLOEXEC)` scheduling.
- OQ3 **WTF SIGUSR1 suspend/resume**: P5.5 extension vs fork-patch variant; decide
  before WK-3 (first multi-JS-thread scenario). Startup asserts are satisfied either way.
- OQ4 **Sysroot `<signal.h>` layout upgrade** (`sa_sigaction` union, `sa_restorer`,
  `siginfo_t`) — L3 consent; blocks any WebKit TU that compiles signal code.
- OQ5 **P2 interplay**: reap status write + parked-caller rewrite + frame write through
  P2's user-pointer helper; confirm with the P2 lane before S2/S3 land.
- OQ6 **FP frame helpers** (`arch_fpu_save_to`/`restore_from` refactor) — x64/ARM FPU
  owners confirm the shape.
- OQ7 **Ratify D9.3** (kill of a secondary tid now targets the group; P1 thread-only
  kill loses public meaning; no in-tree callers).
- OQ8 **`getenv` row ownership**: P5 adds 85 now (so the P5 gate can verify envp
  end-to-end); if the integrator prefers, storage-only + kernel-unit tests + P6.3 adding
  the row is the fallback.

## 7. Risks for the integrator

- **The frame engine (D8) is the largest P5 workstream** and the most arch-sensitive
  (context/FP save formats). It is staged last-but-one (S3) behind SIGCHLD-only
  acceptance; if it slips, routes still work if the reaper is waitpid/WNOHANG-based
  (route B needs no handler delivery) — that hedge is deliberate.
- **SIGUSR1 landmine**: passes startup, unusable at runtime. Flag to L8/WK before any
  multi-threaded-JS experiment; do not let it surface as a mysterious hang in WK-3.
- **Slot pressure**: the 64-PCB table now holds a 3-process browser + transient spawn
  workers; the leak/zombie counters (D5.6) must land with the implementation or the
  failure mode is unobservable on device.
- **Two-row temptation**: any implementer tempted to add `SYS_EXECVE 83` alongside
  `SYS_EXEC 40` must re-read D2 — duplicate surfaces were explicitly rejected.
- **P2 ordering**: D3.6/D5.7 touch the same user-pointer discipline as P2; land P5's
  pointer-using code after P2's helper freezes, or dual-path it as P2 does.
- **glib route A hidden surface**: `F_DUPFD(_CLOEXEC)`, `dupfd_cloexec`, fdwalk-closes —
  only if Route A is chosen; sized in D12/D13, not built in P5.

## 8. Integrator review (consent record — 2026-09-30)

Reviewed at merged base `9a939e5` (P2 S1–S3 on main; the P4 design was
consented in the same review round).  Spot-verified against the tree: the
sysroot `<signal.h>` placeholder status, the `sys/wait.h` status-macro
subset, the `process_kill`-does-not-wake-`WAIT_CHILD` gap (confirmed by
inspection), today's rows 16/39/40 shapes, and the absence of any
FD_CLOEXEC storage.  Verdict: **consented as designed; OQ1's amendment is
applied to §A.1b with this record.**

- **OQ1 A.1b amendment — approved as designed.**  40/39/16 extended in
  place; 83/84/85/86 = `SIGACTION`/`SIGRETURN`/`GETENV`/`SPAWN_EX`; the v1
  provisionals (`EXECVE 83`/`WAITPID 84`/`KILL 86`) are **withdrawn**.  The
  rows are defined in `src/include/syscall.h` **now** (single-writer edit
  ahead of Wave 1f, together with P4's 76–79); `SYS_MAX` is 86 from that
  edit — the P5 gate record adds test evidence, it does not edit `SYS_MAX`.
- **OQ2 launcher route — ratified as recommended:** Route B primary,
  Route A kept viable; the `posix_spawn` adapter is L3 if Route A wins at
  WK-2; `F_DUPFD(_CLOEXEC)` is scheduled with that decision.
- **OQ3 SIGUSR1 deferral — approved** with the trigger (decide before WK-3
  / the first multi-JS-thread scenario; P5.5 per-thread delivery vs
  fork-patch).  The landmine stays flagged for L8/WK.
- **OQ4 sysroot `<signal.h>` upgrade — approved for the P5 impl lane**
  (`union { sa_handler; sa_sigaction; }`, `sa_mask`, `sa_flags`,
  `sa_restorer`, minimal `siginfo_t`); no parallel owner in Wave 1f.
- **OQ5 P2 interplay — resolved: P2 S1–S3 are merged at the 1f base.**
  Route ALL user-pointer reads/writes (reap status write, parked-caller
  result rewrite, signal-frame write, exec argv/envp reads) through P2's
  frozen helpers (`vm_touch` + the per-arch user-range checks in `vm.c` /
  trap.c).  No dual path needed; the design's "until then" clauses are
  void.
- **OQ6 FP-frame helpers — approved** (`arch_fpu_save_to`/`restore_from`
  refactor; F1.5/P1 precedent, mechanical change).
- **OQ7 D9.3 — ratified:** kill of any group member targets the group; the
  thread-only kill branch loses public meaning; no in-tree caller depends
  on the old shape (verified).
- **OQ8 `getenv` row 85 — P5 owns it** (not deferred to P6.3): needed for
  end-to-end envp verification at the P5 gate.
- **Storage-shape note:** the P4 review adopted `uint32_t fd_cloexec`
  (D7's shape) as the canonical FD_CLOEXEC storage — already pre-frozen on
  main; D7's block references it.

Implementation starts in Wave 1f (`browser/l2-p5-impl`) at this review's
base, S1–S2 first (storage/kill/SIGPIPE → reap unification), with the
note's stage-green discipline and the route-B hedge for the frame engine.
