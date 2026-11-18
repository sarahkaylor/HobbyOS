# cp/winefix — Wine-side crashes (lane H3-winefix, wave 8)

Plan: `docs/cyberpunk.md` §7 (wave 8, H3-winefix row).  Wine citations are
`file:line` in the pinned tree (`strix:~/cp/wine-cp/src/wine-11.0`, the
tarball in `~/cp/wine-rt/dl`).  All numbers are from **strix** (Zen 5, KVM,
`-smp 8 -m 12G` guests).  Evidence: `docs/cp-winefix-evidence/`.

## 0. What this lane adds

| piece | file |
|---|---|
| Wine source patches, applied in name order | `tools/cp/wine-patches/*.patch` |
| `CP_WINE_PATCHES=<dir>` in the Wine build | `tools/cp/build-wine.sh` |
| targeted repro of the SIGQUIT race (PE program + launcher + harness) | `tools/cp/p5/quitrace.c`, `tools/cp/p5/qrwine.c`, `tools/cp/p5/run-quitrace.sh` |
| P5 grader names signature W1 (still FAILs, no allowlist) | `tools/cp/p5/boot_wine_p5.py`, `tools/cp/p5/run-wine-p5.sh` |

Patched stage: `strix:~/cp/lanes/winefix-wine/stage/CP/WINE` (build script
`strix:~/cp/lanes/winefix-build-wine.sh` = `~/cp/build-wine-cp.sh`'s flags +
`CP_WINE_PATCHES`, own `CP_WINE_WORK`; 115 s / 185 s builds).  Its file list
equals `~/cp/wine-cp/stage/CP/WINE`'s plus `share/wine/cp-wine-patches.txt`.

## 1. W1: wineserver's SIGQUIT lands in a thread that is already exiting

P5 red: `[KERNEL] pid=… (services.exe) memory fault VA=0x3A8
PC=quit_handler+0x40` (C6-p7c's analysis,
`git show cp-p7c:docs/cp-p7c-evidence/p5-services-quit-handler-fault.txt`).
Checked against the source; it is correct, with one precision:

- `server/thread.c:1608-1631` `kill_thread()`: a thread not in a server wait
  gets `send_thread_signal( thread, SIGQUIT )` when `violent_death`;
  `terminate_process` and `terminate_thread` (`:1809-1821`, for another
  thread) kill violently.
- A thread that exits **on its own** is not removed then: `terminate_thread`
  on self only replies `self = 1` (`:1817`); the thread stays listed until the
  server processes its request-fd hangup.
- `dlls/ntdll/unix/thread.c:1494-1513` `exit_thread()` and `:1474-1479`
  `abort_thread()` block `server_block_set` only, which is
  `server.c:1626-1633` = ALRM IO INT HUP USR1 USR2 CHLD: **no SIGQUIT**.
- `pthread_exit_wrapper()` (`thread.c:1105-1113`) closes the server fds and
  calls `pthread_exit()`; glibc 2.31's `start_thread` then runs
  `__nptl_deallocate_tsd()`, which sets every TSD slot to NULL (whatever the
  key's destructor), then `madvise(MADV_DONTNEED)` on the stack, then exit.
- `NtCurrentTeb()` on the unix side is `pthread_getspecific( teb_key )`
  (`thread.c:1634-1637`).  `quit_handler()` (`signal_x86_64.c:2608-2614`)
  calls `is_inside_syscall()` -> `ntdll_get_thread_data()->kernel_stack`
  = `NtCurrentTeb()+0x3a8` -> NULL+0x3a8.
- Precision: the TEB itself is **not** freed at that point (`exit_thread`
  frees the *previous* exiting thread's TEB after `pthread_join`); what is
  gone is the TSD slot that `NtCurrentTeb()` reads.

The same window has a second, silent outcome that the targeted repro hit
first: a SIGQUIT that arrives *before* the TSD is cleared (inside
`exit_thread`: `pthread_join`/`munmap` of the previous TEB, the fd closes)
runs `quit_handler -> abort_thread()` a second time: `nb_threads` is
decremented twice and the server fds closed twice.  With enough of those
`nb_threads` reaches 0 while threads still run and `abort_process(0)`
`_exit(0)`s the whole process (stock run `stock2`: SIGQUIT at
`ipc=munmap+0xb`, then the process exits code 0 in the middle of its work).

**Fix, patch 0001** (`tools/cp/wine-patches/0001-ntdll-block-sigquit-in-thread-exit.patch`):
`exit_thread()` and `abort_thread()` block `server_block_set` + SIGQUIT
(new `block_exit_signals()`).  The thread is exiting anyway; the pending
SIGQUIT is discarded with it.  `exit_process()` is unchanged.

**Upstream:** only Wine 11.0 is on strix and nothing was downloaded, so
whether later Wine changed this is not checked.  To check, the tarball would
be e.g. `https://dl.winehq.org/wine/source/11.x/` (latest 11.x; needs the
integrator's approval).

## 2. W1b: SIGQUIT in a new thread before it has a syscall frame

Found by the repro's `proc` mode on the stock stage:
`[KERNEL] pid=… (quitrace.exe) memory fault VA=0xA8 PC=0x108028FD58 … inH=1`.
`[SIGX]` gives the SIGSEGV handler `h=0x1080294120` = `segv_handler`
(`nm` 0x3f120) -> ntdll.so base 0x1080255000 -> PC = +0x3ad58 =
`user_mode_abort_thread` entry, `mov 0xa8(%rsi),%rbp` with `rsi` = frame = NULL.

- x86_64 `signal_start_thread` (`signal_x86_64.c:3003-3010`) sets
  `thread_data->syscall_frame` only when it is still 0, i.e. a new thread's
  frame is NULL until then; `start_thread()` (`thread.c:1121-1133`) calls
  `server_init_thread()` first.
- The server knows the thread's unix tid once it has processed
  `init_thread`, so a process termination in that window SIGQUITs it while
  it waits for the reply; `is_inside_syscall( sp )` is `sp <= NULL` = FALSE,
  so `quit_handler` calls `user_mode_abort_thread( 0, NULL )`.

**Fix, patch 0002** (`tools/cp/wine-patches/0002-ntdll-quit-handler-no-syscall-frame.patch`):
a thread with no syscall frame is still in its unix-side start: call
`abort_thread()` directly (the "inside a syscall" path).  Its fds are valid
or -1 (`virtual.c:4023-4027`), `nb_threads` was counted by the creator.

## 3. The repro (`tools/cp/p5/run-quitrace.sh`)

`run-thrstorm.sh`'s staging (MODE=test kernel, console Wine over a temporary
localhost-only NFS export, one QEMU slot per boot), launcher `qrwine.c`:
`wineboot -i`, then `QR_REPEAT` x the runs of `QR_PLAN`, `wineserver -k`.

- `quitrace.exe thr <rounds> <pairs>`: each killer thread creates a worker,
  the worker sets a flag and calls `NtTerminateThread(self)`; the killer
  waits for the flag, spins 0..255 us and `TerminateThread()`s it (the
  server SIGQUITs it if it is still listed).  The worker calls
  `NtTerminateThread` directly, not `RtlExitUserThread`: a kill during
  `LdrShutdownThread` orphans the loader lock (first version hung, run stock1).
  A worker that does not start within 20 s is reported `[QR] thr STUCK`.
- `quitrace.exe proc <n> <ms_max>`: the P5 shape — start
  `quitrace.exe churn` (16 threads creating self-exiting threads forever),
  `TerminateProcess()` it after 150..ms_max ms.
- Grade: Q1 every run `[QR] done rc=0` + exit 0, Q2 no fault line, Q3
  `wineserver -k` clean; `[QRG] runs= ok= faults= va3a8= vaa8=`.

Results: §5.

## 4. Log mining (every Wine log on strix)

`find ~/cp` for `*.serial`, `serial*.log|txt`, `*.gate`, `*serial*`: 8815
files, 3120 unique by sha256, 884 mention Wine.  Signatures counted per boot
(file stem) and per distinct line (copies collapse).  Full table:
`docs/cp-winefix-evidence/log-mine-table.txt`; triage in the H3 report
(§6 there).  Wine bugs fixed here: W1, W1b.  Latent Wine bug found but not
patched (no repro): W2, `server/request.c:413` `receive_fd()` calls
`grab_object( get_process_first_thread( process ))` with no NULL check; after
`Protocol error: … recvmsg: Too many open files` killed a process,
wineserver crashed there (`PC=wineserver+0x43d20 = grab_object`, VA 0, 5
boots, all 2026-11-17 00:00-00:40 in `lanes/game`, before C1-k5's fd scaling;
none since).

## 4b. W3: TEB exhaustion under mass TerminateThread (test artifact, open)

Patched stage, `thr:5000:4` (20000 kills per run), first run after
`wineboot`: 3 of 3 boots,
`memory fault VA=0x0000001371EE2030 PC=ntdll.so+0x614ef` = `init_teb`
(from `virtual_alloc_teb`, `virtual.c:4105`), 27-30 faults per run at slots
0x10000 apart.  `[THRS]` before it: `regs=9041 anon_mib=9468` in the
quitrace process.  A thread killed by `TerminateThread` exits through
`abort_thread()`, which never frees its TEB, PE stack or kernel stack (only
`exit_thread()` frees the *previous* exiting thread's).  So 20000 kills leak
about 9 GiB of anon mappings and the TEB area runs out.
`virtual_alloc_teb()` ignores the status of its `MEM_COMMIT` call, so
`init_teb` writes into a block that is not mapped (`in=HOLE`; the TEB pointer
0x1371EE2000 is above `user_space_wow_limit`).  Stock Wine has the same code;
its `thr` runs never get that far (they crash or hang within seconds).
Not a real-workload issue (Cyberpunk does not kill threads by the
thousand).  The repro's thr plan is cut to `thr:2000:4` (8000 kills), which
stays clear of it.  Open, not patched: whether HobbyOS's reserve with
`user_space_wow_limit` really returned an address above the limit (kernel)
or Wine took it from elsewhere was not traced.

## 4c. H2-F2: `wineserver -k` ignored for 10.5 s, then SIGKILLed (triage: kernel)

H2-early's repro loop (`git show f0ca9786:docs/cp-early-evidence/repro-loop-strix.txt`):
7 of 12 `wineserver -k` took 10.5 s and ended with `wineserver exited
(signal 9)`, leaving 8 clients blocked each time.
- Wine side is correct.  `wineserver -k` = `server/request.c`
  `kill_lock_owner(-1)`: `kill(pid, SIGINT)`, then polls the lock
  20 times with `usleep(50000 * i)` = 10.5 s, then `SIGKILL`.  10.5 s
  therefore means the server **never acted on the SIGINT**.  The server
  installs `do_sigint` (`server/signal.c:207`, `:302`), which writes a
  byte to its handler pipe; the main loop then calls
  `shutdown_master_socket()` (`server/process.c:597`), which kills every
  process and exits within at most 2 s.  The main loop does not block
  SIGINT (`sigprocmask` appears only around `change.c:270-284`).
- Where the server was: `[EXITPC] pid=202 last_user_pc=0x108012764A` =
  wineserver's libc (base 0x1080008000) + 0x11f64a = `epoll_wait`.
  `[THRW] pid=202 … blk 232:0.92`, i.e. parked in syscall 232.  glibc 2.31
  has no `epoll_pwait2` wrapper, so `server/fd.c:611` plain `epoll_wait`
  is what runs.  On Linux a SIGINT with a handler interrupts it with EINTR
  at once.
- The kernel's park (`src/kernel/linux_compat_timers.c`
  `l_tmr_epoll_wait_common`) checks `linux_compat_sig_deliverable()`
  before every park and re-runs the syscall at least every 250 ms.  So a
  queued, unmasked SIGINT should be seen within 250 ms.  That it was not
  seen for 10.5 s means one of: the SIGINT was never queued to that
  group; it was queued while wineserver's mask had SIGINT set (e.g. a
  handler's `sa_mask`, which includes SIGINT, not restored by
  `rt_sigreturn`: compare the `[SFX] MISMATCH` rows); or
  `sig_deliverable` did not count it.  The serial has no SIGINT send or
  delivery rows (`sig=2` appears 0 times), so this needs a probe.
- The 8 leftover clients (services, winedevice, plugplay, svchost,
  explorer, rpcss, vkcube) are a consequence.  A clean `-k` SIGQUITs
  every client; a SIGKILLed server does not, and clients parked in ntsync
  waits get no wake from the server's death.
- **For H4-kglitch:** a native probe with a SIGINT handler that writes
  a pipe, blocked in `epoll_wait(-1)` on that pipe; the parent sends
  `kill(SIGINT)` 100 times and expects EINTR plus the handler within
  10 ms each.  Variants: (a) after the child has run and returned from a
  SIGCHLD or SIGIO handler whose `sa_mask` contains SIGINT, then check
  `SigBlk` in `/proc/<pid>/status` or `rt_sigprocmask`; (b) a child that
  holds an `F_SETLK` lock, with the pid read back through `F_GETLK`, as
  `kill_lock_owner` does.  Not fixed here.

## 5. Results (strix, KVM, `-smp 8 -m 12G`)

Repro A/B (`docs/cp-winefix-evidence/quitrace-ab.txt`), 10 boots that
alternate stock (`~/cp/wine-cp`) and patched (`~/cp/lanes/winefix-wine`),
each running 3 x `thr` + 3 x `proc`:

| | stock (5 boots, 30 runs) | patched (5 boots, 30 runs) |
|---|---|---|
| boots with a W1/W1b fault, hang or silent exit | **5/5** | **0/5** |
| `thr` runs OK | **0/15** (by exit status: 7 hung = watchdog exit 3, 6 silent exits with status 0 and no done row, 2 early exits with status 1 / -1) | 12/15 (12/12 at `thr:2000:4`; 3 W3 failures at `thr:5000:4`) |
| `proc` runs done rc=0 | 15/15 (but faults on the console) | 15/15 |
| W1 `memory fault VA=0x3A8 PC=quit_handler+0x40` (the P5 signature, same PC 0x1080293940) | 4 events in 2 boots | 0 |
| W1b `memory fault VA=0xA8 PC=user_mode_abort_thread` | 16 events in 4 boots | 0 |
| W3 `init_teb` (TEB exhaustion, §4b) | not reached | 3 boots, only at `thr:5000:4` |

Gates with the patched stage (`docs/cp-winefix-evidence/gates.txt`), tree
`strix:~/cp/lanes/winefix-g`: P2 PASS (G1/G2/G3); P4 rc=0; P5 x3 PASS
(vkcube, d3d11, d3d12, fullscr; 0 fault lines; 68-70 SIGQUIT deliveries
per boot); P8 pfx rc=0; P8 Venus main menu OCR at 292-306 s.  This matches
integ-gates15 (P2 PASS, P4 rc=0, P5 2/2 PASS, P8 Venus main menu).  The P5
W1 base rate (about 0.7 %/boot, 3 in about 440 boots) is too low for 3 boots
to show a difference; the repro above is the before/after evidence.

P5 grader: unchanged strictness.  A W1 line still fails the gate; the grader
also prints `known signature W1 … stage LACKS|HAS the 0001 SIGQUIT patch`
(checked by regrading `integ14/p5.2.serial`: FAIL, plus "LACKS").
