# cp/thr — threads and signals the way glibc 2.31's NPTL and Wine 11 need them (lane C1-thr, wave 3)

Plan: `docs/cyberpunk.md` §6 P2, §7.2c (C1-thr row).  Evidence:
`docs/cp-thr-evidence/` (strix, Zen 5, QEMU/KVM).  Probes:
`src/user/linux_probe_thr.c` (new) and `src/user/linux_probe_join.c` (rows A
and E fixed).  Reference kernel for every probe row: strix itself (Linux
7.0.0-43), same static-PIE musl ELF.

## 0. Summary

| Item | Root cause found | Fix |
|---|---|---|
| join row E (`clone failed errno=22`) | **musl 1.2.5's `clone()` wrapper** refuses `CLONE_THREAD` in user space (`EINVAL`); the row fails identically on Linux 7.0. The kernel never saw the call. | Row E issues a raw `clone(2)`; the kernel's thread clone already worked. `clone(2)` now follows Linux's flag rules (§1). |
| join row A (`wait=0 wake=-1 flag=1`) | The **probe's** race: the setter stores `wake_count_` *after* its `futex(WAKE)` returns, and the woken waiter read it first.  The kernel's own WAKE record said `woke=1`.  It showed under HobbyOS because the waker spent milliseconds printing a `[FUTEX]` row inside the wake syscall. | The row reads the count after `pthread_join`; each row is one `write(2)`. |
| ~100 ms (here 10–13 ms) tgkill → handler on a parked thread | Not a tick recheck: **~2 KB of synchronous UART diagnostics** on the send / interrupt / plant / sigreturn path (`[SIGIN] [SIG] [TGTK] [FWK] [SFS] [SIGRZ] [FPUA] [SIGP] [SFP] [SIGD] [SIGR] [SFR] [RT]`), two port exits per byte. Proof: `make SIG_TRACE=1` puts the latency back to 9–11 ms. | Those rows are off unless `make SIG_TRACE=1` (`src/include/sig_trace.h`); a running target is kicked with a reschedule IPI (Linux `kick_process`). Latency now 0–1 ms (CLOCK_MONOTONIC resolution) for futex, nanosleep, sigsuspend, read and spinning targets. |
| `[FUTEX]` rows splice into probe output | Per-op `[FUTEX] ##n` / `[IPI]` rows emitted from inside the program's own futex/wake syscalls, between the probe's three `write(2)`s per row. | Gated behind `make FUTEX_TRACE=1`; probe rows are single writes (a single `write` is already atomic against kernel lines: `console_write_user` holds `uart_lock` for the whole buffer). |
| SIGQUIT/SIGUSR1 to one thread | Found while building S1–S6: a thread started with the **pid slot's previous occupant's mask** (musl's `pthread_exit` blocks everything, so a raw clone could start fully blocked); **`sa_mask` was never stored** and the handler ran with only the interrupted mask; handlers **never nested**; **process-directed** signals only interrupted the main thread; a stale `sig_park_kind` made an SA_RESTART signal "restart" a `read(2)` as a `nanosleep`, rewinding the PC into the middle of an instruction (SIGSEGV). | §3. |
| exit / robustness | `exit_group(231)` from a non-leader ended **only that thread** (native `SYS_EXIT`'s carve-out for musl's `exit(60)`); robust-list death kept the TID bits (`0xC0000003`, Linux writes `0xC0000000`), ignored `list_op_pending`, did not check ownership, and woke every waiter. | §4. |

## 1. `clone(2)` (`linux_compat_thr.c` `l_thr_clone`, `process.c`)

Flag words in use (verified, not assumed):

| Caller | Flags | Source |
|---|---|---|
| glibc 2.31 `pthread_create` | `0x003D0F00` VM\|FS\|FILES\|SIGHAND\|THREAD\|SYSVSEM\|SETTLS\|PARENT_SETTID\|CHILD_CLEARTID, ptid == ctid | `libpthread-2.31.so`: `mov $0x3d0f00,%edx` before `__clone` (sysroot `strix:~/cp/sysroot-glibc231`) |
| musl 1.2.5 `pthread_create` | `0x007D0F00` (+DETACHED) | `[LCOMPAT] clone flags=0x7D0F00` on base |
| glibc 2.31 `fork()` | `0x01200011` CHILD_SETTID\|CHILD_CLEARTID\|SIGCHLD | `libc-2.31.so` `__fork`: `mov $0x1200011,%edi` |
| glibc 2.31 `posix_spawn` | `0x00004111` VM\|VFORK\|SIGCHLD, own stack | `libc-2.31.so` `__spawnix`: `mov $0x4111,%edx` |
| glibc 2.31 `vfork()` | syscall 58 | `__vfork`: `mov $0x3a,%eax` |
| Wine 11.0 | threads via `pthread_create` (`ntdll/unix/thread.c:1450`); processes via `fork()` (`server/request.c:860`, `ntdll/unix/process.c:419-635`) and `posix_spawn` (`ntdll/unix/loader.c:319` `build_path_and_exec`, `msv1_0/unixlib.c:201`); no raw `clone`/`clone3` | grep of `wine-11.0.tar.xz` |

`exec_wineserver()` (`loader.c:574-590`) tries `bin_dir/wineserver`, then
`$WINESERVER`, then each `$PATH` entry, moving on when `posix_spawn` returns
an error.  glibc 2.31 reports the child's exec failure through memory the
child **shares** (`args->err`), so a `CLONE_VM` emulated by a copy would make
every candidate "succeed" and the child exit 127.  Hence real sharing:

- **Linux's refusals first, Linux's errno** (copy_process order): NEWNS\|FS,
  NEWUSER\|FS, THREAD without SIGHAND, SIGHAND without VM, THREAD with
  NEWUSER/NEWPID, PIDFD with PARENT_SETTID/DETACHED → `EINVAL`; `SETTLS`
  with `tls >= TASK_SIZE_MAX` → `EPERM`.  Only the low 32 bits are flags.
- **Accepted as no-ops:** PTRACE, UNTRACED, IO, SYSVSEM, DETACHED.
- **Thread** (THREAD): `process_clone`.  Stack 0 is legal (the caller's %rsp).
  The new thread now starts with its **creator's signal mask** and no
  altstack (`linux_compat_sig_clone`, before READY).
- **CLONE_VM|CLONE_VFORK** (posix_spawn, vfork): `process_fork_vm()` — a new
  process (own fd table, dispositions, mask) whose `as` **is the parent's AS**;
  its own empty AS waits in `vfork_own_as`.  The calling thread parks until
  `process_vfork_release()` runs for the child: from `group_teardown` (exit),
  `process_kill_group_sig` (kill) and **`process_exec_current` right before it
  tears the old AS down** (one line in `program_loader.c`, C2-exec's file).
  The park is a *rewound* private syscall (`0x7F00`), so a spurious wake just
  parks again; a signal to the parent is held until the release (Linux does
  the same).  `proc_release_mem_locked` never frees an AS a vfork child still
  borrows.  CLONE_VFORK without CLONE_VM = fork + the same wait.
- **fork shape** (no VM): unchanged C2-sys route to `process_fork()`.
- **Refused with `EINVAL` + one `[LCOMPAT] clone refused` row** (Linux would
  accept; nothing in glibc/Wine asks): namespaces, PIDFD, CLONE_PARENT; a
  thread without FS\|FILES (cwd and fd table are group state here);
  VM\|SIGHAND without THREAD (LinuxThreads); VM without VFORK (a concurrent
  shared-memory process); a process clone with FS/FILES/SETTLS or an exit
  signal other than SIGCHLD.  Probe rows K4, X6, X7 report these.

## 2. Exit and robust futexes

- `exit_group(231)` is in `l_tab_thr` now and always runs `process_exit`
  (whole group).  `exit(60)` keeps its thread meaning (`l_thr_exit`).
- `CLONE_CHILD_CLEARTID`: unchanged contract (clear + wake 1 at thread exit);
  K1/K2/S5 measure a joiner parked first and woken by it.
- Robust list death = Linux `handle_futex_death()`: only a word whose TID is
  the dying thread's; new value `(old & WAITERS) | OWNER_DIED` by CAS (a
  waiter may be setting WAITERS); wake **one** waiter (a PI word is handed to
  its first waiter as before); `list_op_pending` is processed last with the
  "already unlocked, wake one" rule; each entry's `next` is read before the
  word is released.

## 3. Signals

- **Latency** — see §0.  The kick is `sched_kick_pid_cpu()` (reschedule IPI
  to the CPU running the target; the 0x81 handler's schedule/trap exit
  delivers).
- **Park bookkeeping per syscall.**  `linux_compat_dispatch` clears
  `sig_park_kind`/`sig_park_nr` and records `sys_entry_pc` on every compat
  syscall; a BLOCKED park whose saved PC is `sys_entry_pc - 2` is a rewound
  one.  Signal-ends-park rules, all Linux's:
  untimed FUTEX_WAIT restarts under SA_RESTART, a timed one → EINTR;
  nanosleep/clock_nanosleep → EINTR always (they used to restart);
  sigsuspend → EINTR; a rewound read/write/recv/accept/connect/wait restarts
  under SA_RESTART (`sig_park_kind = 3`, frame at the `syscall`), but
  poll/select/pselect/ppoll/epoll_wait/pause → EINTR; a park that is neither
  signal-aware nor rewound (wait4's reap delivery writes the result into the
  saved frame) is **left parked** with the signal pending instead of being
  woken into a garbage return value.
- **Masks.**  `rt_sigaction` stores `sa_mask` (and reports it and the flags
  in `oldact`, which used to read 0).  At handler entry the mask becomes
  `mask | sa_mask | sig` (not `sig` with SA_NODEFER); `rt_sigreturn` restores
  `uc_sigmask` as before.  Handlers **nest** for Linux-ABI threads
  (`sig_in_handler` is a depth); the mask decides.  A fault inside a handler
  nests too unless its signal is blocked (then the default action, as
  `force_sig_fault`).
- **Process-directed** (`kill(2)`): when the main thread blocks the signal,
  the first live member that does not is interrupted / kicked (Linux
  `complete_signal`).  A thread-directed signal (`tgkill`) stays on that
  thread only.

## 4. Trace knobs

| knob | rows |
|---|---|
| `make FUTEX_TRACE=1` | first 48 `[FUTEX] ##n` ops, first 8 `[IPI]` kicks |
| `make SIG_TRACE=1` | the per-signal rows listed in `src/include/sig_trace.h`, `[TEXIT]`, `[FUTEX] exit/robust`, `[LCOMPAT] clone flags=` |

Both knobs are part of the `obj/<arch>/.mode` stamp, so toggling one
rebuilds the kernel.  The futex ring, the hang dump and every anomaly row ([SIGBLK], [SIGB]
blocked, [SKW], [SIGPL] FAIL, fault rows) are unchanged and always on.
