# cp-kglitch — kernel-side glitches (H4-kglitch, wave 8)

Base: integ15 = a23bfbeb. Host for every number: strix (Zen 5, KVM, `-smp 8 -m 12G`).
Evidence: `docs/cp-kglitch-evidence/`.

## 1. Torn and missing console rows under host load

### 1.1 What tore them: the guest's wire lock, after a host stall

The `console` probe failed on integ12 (10 torn / 72 missing / 1 interleaved) and on
unmodified integ11 trees of two lanes (8/73, 6/56, 6/45), and passed when strix was
quieter. Every failing log carries the same two fingerprints:
- the summary row's `forced=` counter is non-zero: 10, 4 and 1 in the three failing
  logs left in lane trees on strix, and 0 in all five passing ones
  (`console-logs-census.txt`; integ12's own log was overwritten by its reruns);
- the shredded rows are made of several `[CONS!] ` tags written at once, byte by byte:
  `…4B[[[[COCI[CCNCSTPSWONSO!dRSkrNyT5CS!]`. That is four or more CPUs that each took
  the wire over (`wire_lock() == 0`) and wrote at the same time.

QEMU's serial file backend is not the culprit. Port writes are serialized under the
BQL, the 16550 FIFO drains synchronously into the file, and no byte is lost there.
Everything torn comes from more than one guest CPU being on the wire at once.

How a host stall gets there (`src/kernel/kconsole.c`, integ15):
1. The console lock is a ticket lock. A waiter that sees no progress for
   `CONS_WAIT_US` = 200 ms of **wall** time takes the wire over and writes, tagged
   `[CONS!] `.
2. When the host stops the guest (SIGSTOP-like: page-cache or dirty-throttle stalls in
   QEMU's I/O thread, vCPU threads descheduled under load), the guest clock (TSC) keeps
   running. The holder and every waiter are frozen together. On resume every waiter's
   next clock sample says 350 ms have passed with no progress.
3. Every waiter took over at once. A takeover moved `wire_serving` to the taker's **own**
   ticket, which also released every ticket below it. So after one stall, all queued
   CPUs (and the resumed holder) wrote at once.
4. A 50 ms "stale partial line" rule had the same blind spot. A preempted thread's half
   row looked 400 ms old after a stall and was pushed out alone (a tear, no soup).

### 1.2 The fix

- **Watched time only.** The takeover bound counts only time the waiter itself
  observed. A gap between two of its clock samples longer than 2 ms means it was not
  running (the host stopped it, or the whole guest), and that gap is not counted.
- **Owner progress.** The owner bumps `wire_beat` once per byte. Any beat restarts the
  waiter's clock, so a slow but live owner is never taken over.
- **Bound.** The normal bound is now 2 s of watched no-progress (fatal paths keep
  20 ms). A 10 s wall-clock backstop covers a clock that never stops looking stalled.
- **Head-only, one-turn takeover.** Only the waiter whose ticket is `serving + 1` may
  take over, and only by one turn. The queue behind it sees the queue move, and its
  clock restarts.
- **A usurped owner notices.** It checks before every byte that the wire is still its
  own. If it is not, it queues again and writes the rest of its row on a new line,
  tagged `[CONS!] `. A usurped row is cut in two. It is never shuffled into the
  taker's bytes.
- **Stall-clipped ages.** CPU 0's 100 Hz tick sees the guest's clock jump. The excess
  goes into `cons_stolen_us`, which every console age (slot staleness, the quiet wire)
  subtracts.
- **Diagnostics.** A rare `[CONS] takeover cpu= holder= why= watched_us= wall_us=
  gaps= maxgap_us=` row records each takeover. The summary row gains `usurped=`,
  `stalls=` and `stolen_ms=`.
- **C1-k7 rule.** The per-CPU wire state (ticket, tag, bound) is one 64-byte record
  per CPU (`struct cons_wcpu`), not new `[MAX_CPUS]` scalars.

### 1.3 Tests that failed before the fix

- `kconsole_mt_test_host` (new, in `make host_tests`). Six threads, one per "CPU",
  print through the real `cons_kwrite`/`cons_user_write` onto a shared byte wire. The
  holder stalls in the middle of a row in three ways:
  - `jump`: the clock jumps 400 ms while the holder is frozen (a host stall);
  - `short`: the holder alone is stuck for half the bound;
  - `long`: the holder alone is stuck for 2.5× the bound.

  It is built with a 300 ms bound. On the integ15 `kconsole.c` the `jump` phase fails
  3/3 (5 forced takeovers, 11–18 rows torn or shredded), and `long` fails 3/3 (soup).
  On the lane, all four phases pass 5/5. `long` loses exactly the usurped row, as two
  `[CONS!]` fragments.
- `kconsole_test_host` T14: a preempted thread's half row survives a 400 ms host stall
  that CPU 0's tick sees. integ15 pushes it out as a stale piece (FAIL); the lane
  passes.

### 1.4 In the guest: before and after under the same load

- **Deterministic reproduction.** `kglitch-work/stall.sh` SIGSTOPs this lane's own
  QEMU for 350 ms every second, from 20 s after start until it exits (no CPU burner).
  Lane and base boots ran in pairs, at the same time, under the same host load. See
  §7 for the commands; the rows are in `console-ab-strix.txt`.
- **Natural load.** Strix itself was loaded (load 10–35, 2–4 guests of this and other
  lanes, P9 game boots, builds). Base runs are this lane's base boots plus integ16's batch
  row; lane runs are the gate batches.

| strix, console probe, 60 s | stall pattern | boots | PASS | torn / missing (sum) | forced |
|---|---|---|---|---|---|
| base (integ15), whole-guest stalls | 350 ms / 1 s | 4 (c, st350-1..3) | **0/4** | 272 / 2067 (89/572, 51/437, 67/611, 65/447) | 38, 48, 4\*, 47 (\*the last summary row came early) |
| lane, whole-guest stalls | 350 ms / 1 s | 3 (st350-1..3) | **3/3** | 0 / 0 | 0 (no `[CONS!]` byte in any log) |
| base, no injected stall (load 10.1–10.8) | — | 2 (a, b) | 1/2 | b: 11 / 117 | b: 14 |
| base, integ16 batch (natural) | — | 1 | 1/1 | 0 / 0 | 0 |
| lane, gate batches (natural, load 15–35) | — | 2 | **2/2** | 0 / 0 | — |

Under natural load the base fails only now and then: 1 of 3 boots here, and 4 of 9
lane-tree logs on strix before this lane. So the natural-load rows alone cannot prove the
fix. The stall A/B does: base 0/4, lane 3/3, with the same host load in each pair.

## 2. `safe_wfi` halts in the world walk

C1-k7 counted `safe_wfi` as "a halt on behalf of a claimed slot (a syscall or kernel task
waiting on a device)". It found 6.1–7.3 % of world samples in two lane boots, against
1.0–1.8 % elsewhere. fpsprobe's sampler records no stack words, so whose halts they
were was not known.

**One stack-sampled boot.** `tools/cp/kglitch/fpsprobe-stack.p9` is fpsprobe.p9 with
`prof 60 10 fp-world stack`. Run kg-stack1: strix, Venus, the lane kernel, 9070 XT
busy median 34 %, one other guest. Details in `safe-wfi-kg-stack1.txt`.
- `safe_wfi` was 1.0 % of 4480 vCPU samples (45 samples): 42 had `start_scheduler`
  as their only kernel caller word, and 3 had `flusher_task`.
- The 42 are the **idle loop**: `start_scheduler`'s `idle_claim_check();
  while (1) safe_wfi();`. `sched_wfi_bound` returns at once there, because no pid is
  claimed. Only the ext2 flusher's 50 ms nap (`ext2_vfs.c flusher_task`, a kernel task
  whose `while (now - t0 < 50) safe_wfi();` wakes every 1 ms under C1-k6's bound) is a
  claimed-slot halt.
- QEMU's `info registers` reports HLT=0 for a vCPU it catches waking from (or
  halt-polling in) the `hlt`. So the `safe_wfi` symbol is mostly **idle time that is
  being woken**. Its share grows with the number of idle wakeups, not with any task's
  wait.

**Verdict.** The C1-k7 spike is neither a lost wakeup nor a task-latency glitch: no
claimed slot sits in `safe_wfi`, apart from the flusher's nap. The two high boots are
consistent with more idle-CPU wakeups per frame. One of them, k7l-3, had the GPU 71 %
busy from another guest, so its frames were GPU-bound and its CPUs idle-woken more
often. Their serial logs and profiles no longer exist (the tree was recycled), so this
was not re-measured on them, and nothing was changed.

The flusher's nap holds a CPU claim for up to 50 ms at a time while it does nothing. A
kernel task is preempted by the 0x81 tick (docs/cp-console.md §2.2) when other work is
READY, so it costs at most a tick of latency; a timed park would free the CPU outright. That is filed as finding K12 (perf, open).

## 3. Park sentinels that equal an errno (`GPU_ABI_PARKED == -2 == -ENOENT`)

`GPU_ABI_PARKED` was -2, which is -ENOENT. The compat ioctl route answers it with
`l_park` (the ioctl restarts on wake), so any -ENOENT that reached
`linux_compat_fd.c`'s `/dev/gpu` branch would restart the ioctl for ever. That is the
loop C4-audio hit with its own -2. `gpu_abi.c` passes the raw rc through from the
capset, blob-map, `gpu_m2_map_user` and scanout helpers. None of them returns -ENOENT
today, so it was safe by luck, not by construction.

- `GPU_ABI_PARKED` is now -100001 (`LSND_PARKED` stays -100000).
- `linux_compat_fd.c` carries a `_Static_assert` that both sentinels are below -4095
  and differ from each other.
- The unit test `gpu_abi_m2_test.c test_park_sentinels` (unit image, both arches)
  checks the same at run time; with -2 it fails.

The audit (every park return and every park test in `src/kernel`, both arches) is in
`docs/cp-kglitch-evidence/park-audit.txt`.
- **Named sentinels.** `GPU_ABI_PARKED` was the only one equal to an errno.
  `LSND_PARKED` = -100000 and `L_PI_PARKED` = +1 are safe.
- **The native `-2` park convention** (fs.c, pipe.c, unix.c, eventfd, signalfd and
  timerfd return -2 after setting the process BLOCKED; about 25 testers in the compat
  fd route and both trap.c files). Every tester's callee chain was followed. The only
  real -ENOENT that reaches a `== -2` test is unix `connect()` to a missing path, and
  its three testers already disambiguate (non-blocking probe in `l_connect`; a
  BLOCKED-state check in both arches' native connect). Moving the whole native
  convention (79 sites) to a non-errno value is a wide change for no live bug, so it was
  not done here (§8).
- **Found on the way** (open, not fixed here; finding rows K8–K11 in the report):
  - `-1` is read as EBADF where a real -EPERM (a sealed memfd write) arrives;
  - raw `-1` is passed through as -EPERM by writev/readv/pread64/pwrite64;
  - `l_connect` sets O_NONBLOCK for every family, so a blocking non-loopback TCP
    `connect()` returns EINPROGRESS;
  - `net_socket_accept_lo` returns the park -2 without setting BLOCKED, so a blocking
    loopback `accept` spins in a yield loop instead of sleeping.

## 4. Harness glitches

- **`make host_tests` stopped at the first red recipe line.** `make -k` keeps going
  across targets, not across the lines of one recipe. The python suite (known red: the
  two gaming.md tests) ended the recipe, and about 60 test binaries after it were built
  but never run. Among them were AUDIO_STUB_TEST, LINUX_SND_PARAMS_TEST, the libc
  parity tests, the UI tests and the ARM-free kernel host tests.
  - Every recipe step now goes through `tools/host/host_try.sh`. It applies the same
    40 s timeout HOST_RUN used, records a failing step in a per-make-process file
    (`obj/host_tests.fails.$PPID`, so GB-085's concurrent runs never share it) and
    carries on.
  - The last line prints `[host_tests] summary: N step(s) failed:` with each failure,
    and exits 1 if there was one.
  - **Result** (strix, `host-tests-strix.txt`). Every step now runs, about 3 660
    output lines after the python suite that the gate never produced before. The only
    red step is still the python suite, now 40/42: the two gaming.md tests, plus this
    lane's new self-test case. Every binary after it passes, AUDIO_STUB_TEST (4/0) and
    LINUX_SND_PARAMS_TEST (24/0) included. `make host_tests` exits 1 with
    `[host_tests] summary: 1 step(s) failed: python3 tools/t5/run_t5_tests.py`.
- **`tools/rw/rw_gameplay_test.py` (from main, b8d79881) matches `run_t5_tests.py`'s
  `tools/*/*_test.py` glob.** It boots a guest and drives RimWorld, so on a tree that
  has it, host_tests would go red or boot a VM. It is now excluded next to
  `run_doom_elf_test.py` (GB-176).
  - The grader's self-test builds a throwaway tree with both harnesses and a plain test,
    and checks that discovery finds only the plain one. Without the exclusion it fails.

## 5. Stress soak

The concurrency probes were looped on strix in this lane's trees
(`kglitch-work/soak.sh`, one slot per boot, logs kept for failures only).
- **Trees.** "Pre-fix" means integ15 (`kglitch-s1`) or the console fix alone
  (`kglitch-s3`, 63b6ca73). "Lane" means `kglitch-s4`/`-s5` at 96b626ed or later, plus
  the gate batch's single probe row (`gates-probes-strix.txt`).
- **Concurrency.** Boots ran while 0–3 other guests were up: this lane's gate batch and
  the other lanes' guests (`guests_at_end` in `soak-strix.txt`). Host load was 10–35.

| probe | pre-fix trees (integ15 s1, console-only s3): fails/boots | lane (s4, s5, gate batch): fails/boots |
|---|---|---|
| pipestorm | 0/3 | 0/4 |
| mmap_race | 0/3 | 0/4 |
| fdrace | 0/3 | 0/4 |
| forkcow | 0/2 | 0/4 |
| futex_sig | 0/2 | 0/4 |
| exitgroup | 0/2 | 0/5 |
| manythr | 0/2 | 0/4 |
| sysc | 0/2 | 0/4 |
| ntsync | 0/2 | 0/4 |
| execthr | 0/2 | 0/4 |
| killgrp | 2/4 | 0/5 |
| e2stress | 0/2 | 0/2 |
| fatcoh | 0/2 | 0/2 |
| exitrace | 3/3 | 0/6 |
| sigint | 1/1 | 1/3 |
| sigret | 1/1 | 0/1 |
| badelr | 1/1 | 0/2 |
| fpu_sig | 0/0 | 0/2 |
| tramp_sig | 0/0 | 0/3 |
| trapsig | 0/0 | 0/3 |
| siginfo | 0/0 | 0/3 |
| sigfd | 0/0 | 0/3 |
| timers | 0/0 | 0/3 |

Notes on the table:
- exitrace also had two manual base runs (`exitrace-base-2.gate` and a 150-round first
  version), both FAIL, and one manual lane run (`exitrace-lane-1.gate`), PASS. With them,
  pre-fix is 5/5 FAIL and the lane is 0/7.
- The pre-fix sigint failure is the I6 `[SFX]` console check; I1–I9 pass.
- The lane's one sigint "failure" (s5 #1) was the checker matching the probe's own
  "no [SFX] MISMATCH" text. Regraded with the fixed checker it is PASS: 0 rows.
- sigret and badelr fail on integ15 by design: they are the tests for §6.2 and §6.3.

**Failures found:**
- **K13 (fixed, 96b626ed): waitpid ECHILD for a leader-exited group that exits.**
  - killgrp K6 failed 2 of 4 pre-fix boots: `[LP] K6 a worker's exit_group(7)
    waited=-1ms status=0x2a00 -> FAIL`, with `[WAITPID] ECHILD caller=2 want=12
    children=0 slotstate=0 slotparent=2`.
  - The cause is in exit_group and the last-thread exit:
    1. They set the anchor's `live_threads = 0` and drop proc_lock before
       `group_teardown` makes it EXITED (exit_group's claim drain can take a tick).
    2. With the leader gone (pthread_exit in main, which every Wine process with a
       quitting main thread does) the anchor is THREAD_DONE.
    3. All three reclaimers free a THREAD_DONE anchor with `live_threads == 0`: the
       idle CPUs' `proc_reap_parked_locked`, `thread_alloc_slot_locked` and the
       process-slot scan.
    4. So the child vanished from waitpid, and its memory was released mid-teardown.
  - Fix: the reclaimers also require `exit_owner == 0`, and the last-thread path takes
    `exit_owner` the way exit_group's owner does.
  - New probe `linux_probe_exitrace`: 300 rounds per case, tight WNOHANG polling. X1
    gets echild = 1 of 300 in every pre-fix boot (5/5 boots FAIL) and 0 in every lane
    boot (0/6). killgrp: pre-fix 2/4, lane 0/4.
- **Everything else: 0 failures.** That is pipestorm, mmap_race, fdrace, forkcow,
  futex_sig, exitgroup, manythr, sysc, ntsync, execthr, e2stress and fatcoh, on both
  sides. `exitgroup` X7, a known red under load, did not fail in 5 boots.
- **Not run: thrstorm (P5-style Wine).** The slots went to the gate batch and to the A/B
  runs.

## 6. The integrator's kernel items from H3-winefix's triage

### 6.1 H2-F2: `wineserver -k` ignored for 10.5 s

`linux_probe_sigint` (new) copies wineserver's signal setup (`server/signal.c init_signals`):
SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1 and SIGIO handlers, every `sa_mask`
holding all of them, `sa_flags` 0, and each handler writing a byte to a pipe in the epoll
set. The main loop is a raw `SYS_epoll_wait` (glibc 2.31's).

- **What it sends.** The parent sends SIGINT and then SIGCONT, as `kill_lock_owner` does,
  and expects the handler's byte back.
- **Variants:**
  - I1: `epoll_wait(-1)`;
  - I2: SIGINT sent while a 5 ms SIGUSR1 handler runs, so it is masked by `sa_mask`;
  - I3: the mask read back at the top of every loop;
  - I4/I5: the pid from F_GETLK;
  - I7: a busy second fd and a 50 ms timeout;
  - I8: as I7, plus a thread parked in FUTEX_WAIT;
  - I9: as I7, plus 96 more idle pipes in the set, past the partial-scan threshold.
- **Result** (strix guest, `-smp 8`): I1–I9 pass on integ15 (1 boot, after the strix
  host run) and on the lane (1 boot). Every SIGINT was handled, the worst latency was
  4.0 ms (I2, which includes the 5 ms handler), and SIGINT was never left in the mask.
  F_GETLK names the holder.
- **Conclusion.** The kernel delivers the SIGINT in every shape tried. wineserver's own
  path is: `do_sigint`, then `do_signal` (it writes the pipe only `if
  (!handler->pending)`), then the main loop reads the pipe, then `shutdown_master_socket`.
  So the 10.5 s needs either the SIGINT never reaching the server's group, or the handler
  pipe never being reported by epoll. Neither reproduces outside the game session. The
  repro serial has no `[SIGIN]`/`[SIG] send` row for it, because those rows' budgets were
  spent long before. Still open: the next step is a capped, per-signal-number send/deliver
  row for SIGINT alone (2 = rare), in the boot that runs H2's loop. That row is cheap to
  add, but it is not in this lane's boots.

### 6.2 `rt_sigreturn: bad rip/rsp`, Wine's "unexpected trap 128", `[SFX] MISMATCH`

- **Cause** (run11-venus, `serial.txt`):
  1. A game thread jumps into a hole (`memory fault ... in=HOLE`), and SIGSEGV is
     delivered to Wine's handler.
  2. The handler returns through `rt_sigreturn` with RIP still at the unmapped address.
  3. The kernel's `l_user_ok(rip)` refused that frame and raised SIGSEGV from inside the
     syscall. `l_sf_fill_gregs` writes `tf->vector`, here the syscall frame's 0x80, as
     `REG_TRAPNO`.
  4. Wine's `segv_handler` switches on REG_TRAPNO: `Got unexpected trap 128`. The second
     frame lands on the signal stack the first one still occupies: `nested exception on
     signal stack`.

  Linux does not validate the restored RIP/RSP. The thread simply faults at the new RIP,
  with trap number 14.
- **Fix.** Refuse only what iretq cannot take (a RIP/RSP at or above
  0x0000800000000000), and give that SIGSEGV trap number 13.
- **Probe** `linux_probe_sigret`. S2: a handler returns into `0x500000001000`, and a second
  SIGSEGV must arrive with `si_addr` equal to that address and trapno 14. integ15: `S2 ... entries=2 trapno=128 si_addr=0x0 -> FAIL`, with `[LCOMPAT]
  rt_sigreturn: bad rip/rsp -> SIGSEGV`, which is Wine's "trap 128" exactly. Lane:
  `trapno=14 si_addr=0x500000001000 -> PASS`. The strix host's own Linux kernel passes too.
- **`[SFX] MISMATCH`** is the kernel's own frame-restore check, and it is not a restore
  bug:
  - It keeps one plant per pid, so the outer return of nested frames compared against the
    inner plant. That is fixed: the ring of recent plants is searched first (probe I6 and
    `tools/gate/lp_check_sigint.py`). integ15 FAILs I6 with 8 rows `[SFX] MISMATCH pid=2
    ... delta=1824`; the lane passes with 0.
  - The pfx-try2 row (`delta=-1848`) is Wine's handler deliberately rewriting RSP/RIP to
    dispatch the exception on the thread stack, which Linux allows.

### 6.3 `[BADELR]` noise

- **Cause.** The scheduler's plausibility band `[0x70000000, USER_VA_BASE)` flags every
  resume of a compat thread whose RIP is in the game's PE image (0x140000000). It spent
  its 20-row budget on healthy resumes.
- **Fix.** An elr inside one of the slot's own AS regions is plausible
  (`vm_region_find`, only for elrs the band flagged).
- **Probe** `linux_probe_badelr`: two threads spin at 0x140000000 for 1.5 s, and
  `tools/gate/lp_check_badelr.py` fails the run on any `[BADELR]` row. integ15: FAIL (20 `[BADELR] ... elr=0x0000000140000000` rows, the cap); lane: PASS (0).

### 6.4 explorer/plugplay `PC=...9A93 live=0` teardown faults

- **The records.** `memory fault VA=... in=V1 ... live=0`. `in=V1` means
  `vm_handle_fault` found the group's `as` already NULL, and `live=0` means the group's
  `live_threads` was already 0. So a member thread was still running user code after its
  group's address space had been released.
- **The likely mechanism** is §5's exit race:
  - exit_group (or the last thread) sets `live_threads = 0` and drops proc_lock for the
    claim drain;
  - if the leader had exited, the anchor is THREAD_DONE, and an idle CPU's
    `proc_reap_parked_locked` freed it with `proc_release_mem_locked` (as = NULL) while a
    member still ran until its next tick.
- **Coverage.** The 96b626ed fix closes that path.
  `linux_probe_exitrace` X3 (exit_group while a worker demand-faults pages) is the probe
  row. X3 did not reproduce the fault on integ15 in 1 boot (300 rounds). The race window is as
narrow as X1's (1/300 rounds), and the fault also needs the worker to fault inside it. So
this item is "probably fixed by 96b626ed", not shown fixed.

### 6.5 H1-hunt's kernel instrument findings

- **H1-14: the watchdog's false "console silent for more than 20s"** (15/22 hunt boots).
  - Cause: `watchdog_tick` reads `now`. Its own `[NFSIO]` row, or another CPU's print,
    then stamps `uart_last_activity_ms` a millisecond later, and the unsigned
    `now - last` wraps.
  - Fix: `src/include/wd_silence.h wd_console_silent()`. A stamp at or after `now` is
    not silence. trap.c calls it.
  - Host test `wd_silence_test_host` (in host_tests): W2, a stamp 1 ms after `now`,
    fails with integ15's expression (`-DWD_SILENCE_OLD`: "got 1, want 0") and passes
    on the lane.
- **H1-06: `[BADELR]` on the game's PE image.** This is §6.3's fix, done by region
  rather than by skipping compat processes wholesale. An elr in a compat process's hole
  still flags.
- **H1-07: the `[VENUS] fence_wait2 timeout` cap is spent in the first 6 s** (18/18).
  - Every timeout is still counted. Only waits whose caller asked for ≥ 1 s print
    (`gpu_abi_fence_timeout_reportable`, at most 16 rows), and each row carries the
    running `timeouts=` count.
  - Unit test `gpu_abi_m2_test.c test_fence_timeout_reportable` (5 ms poll → 0, 1 s and
    INFINITE → 1).
  - Not shown in a Venus boot.
- **H1-11: the kernel's xrun count only appeared at PCM close.**
  - Fix: one `[SND] xrun n= frames_done= chunks=` row per xrun (the first 32, then
    every 64th).
  - `tools/cp/kglitch/xrun-host-test.sh` runs the kernel's `linux_snd.c` (the
    LSND_HOST_TEST shim, as C4-audio's host test does) under the staged alsa-lib. A
    small player (`xrunone.c`) starves its 100 ms buffer for 400 ms.
  - integ15: `FAIL -- no [SND] xrun row before the close row` (the close row says
    `xruns=1`). Lane: `[SND] xrun n=1 frames_done=14400 chunks=12`, then PASS.
  - This is the instrument for H1-01's dropouts, not a fix for them.

## 7. Commands (strix)

```
# console A/B with host stalls (lane tree + a base tree, booted together)
bash ~/cp/lanes/kglitch-work/cons-ab.sh 3 350 st350
#   = per boot: stall.sh <tree> 350 1000 & t3_linux_probe.sh console (in a slot)
# host tests
make kconsole_test_host kconsole_mt_test_host && ./kconsole_test_host && ./kconsole_mt_test_host
python3 tools/t5/run_t5_tests.py --self-test
# soak (one slot per boot; result rows to kglitch-work/soak-results.txt)
bash ~/cp/lanes/kglitch-work/soak.sh <tree> <N> pipestorm mmap_race ...
# the new probes (graded by their lp_check_*.py where they have one)
LP_TIMEOUT=400 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh exitrace   # sigint sigret badelr
# the safe_wfi stack boot
P9_SCRIPT=tools/cp/kglitch/fpsprobe-stack.p9 P9_RUN=kg-stack1 VENUS_BUILD=$HOME/cp/lanes/present-venus \
  P8_EXPORT=/srv/nfs/cp-kglitch-p9 CP_WINE_STAGE=$HOME/cp/wine-cp/stage/CP/WINE WINE_BUILD=$HOME/cp/wine-cp \
  PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p9/run-p9.sh
# gates: ~/cp/lanes/kglitch-work/gA.sh (build, infra, P2, P5, P8 Venus) + gB.sh (every probe),
# both derived from ~/cp/integ-gates16.sh
```

## 8. Open

- **H2-F2's root cause is not found.** `linux_probe_sigint`'s nine shapes all pass on
  integ15. The next H2 repro boot should carry this branch's `[SIGINT-S]`/`[SIGINT-D]`
  rows, which say whether the SIGINT was queued, to whom, and whether a frame was ever
  planted.
- **The explorer/plugplay `in=V1 live=0` faults are probably fixed by 96b626ed**, but
  this is not shown: X3 did not reproduce them on integ15 in 1 boot.
- **K8–K11 from the sentinel audit:**
  - `-1`/`-EPERM` is mixed up in the write, writev, readv and pread routes;
  - `l_connect` makes a blocking TCP `connect()` return EINPROGRESS;
  - a blocking loopback `accept` busy-yields.

  None is fixed or probed.
- **K12: the ext2 flusher naps in `safe_wfi` holding a CPU claim** (perf). A timed park
  would free the CPU.
- **The native `-2` park convention is unchanged** (79 sites). Its only real collision
  (unix connect) is already disambiguated.
- **The console still takes a holder over after 2 s of watched no-progress.** A vCPU that
  the host deschedules (or blocks in QEMU) for more than 2 s, while its waiters run, can
  still cut one row in two, with a `[CONS] takeover` row naming it. That was not seen in
  any boot here.
- **Soak depth.** The concurrency probes reached 2–5 boots each, not 10, because of slot
  contention. thrstorm was not run.
