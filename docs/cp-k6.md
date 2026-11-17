# cp-k6 — kernel debt under the game (lane C1-k6, wave 6)

Plan: `docs/cyberpunk.md` §7.2f (C1-k6).  Branch `cp-k6` from `cyberpunk` @
058fd3d5.  Evidence: `docs/cp-k6-evidence/`.  Every number is from strix
(Zen 5, `ssh strix`): guests are intel `MODE=test`, KVM `-cpu host`, 8 vCPU,
12 GiB, under `tools/cp/qemu-slot.sh`; "host" means the strix Linux kernel
running the same static musl probe binary.

| # | item | probe | base 058fd3d5 | lane |
|---|---|---|---|---|
| 1 | high-resolution timeouts | `linux_probe_hrtimer` H1-H12, L1-L3 | FAIL (every row; p50 error 9-10 ms) | PASS (p50 19-37 us) |
| 2 | `clock_nanosleep(TIMER_ABSTIME)`, every clock | `linux_probe_hrtimer` C1-C9 | FAIL (ABSTIME `-EOPNOTSUPP`, ids unchecked) | PASS |
| 3 | SIGKILL (and every process-directed signal) to a leader-exited group | `linux_probe_killgrp` K1-K7, C8's `p8killprobe` K1/K2 | FAIL 6/7 | PASS 7/7 |
| 4 | `signalfd` / `signalfd4` | `linux_probe_sigfd` S1-S11 | FAIL (ENOSYS) | PASS 11/11 |
| 5 | `ru_stime` | `linux_probe_rusage` R1-R4 | FAIL R2-R4 (stime 0) | PASS |
| 6 | locked fd installs | `linux_probe_fdrace` D1-D3 | FAIL (D1: 1764 duplicate fd numbers in 19200) | PASS |
| 7 | `[FRAME-DFREE]` tripwire | unit `frame_test.c` | no counter, no row | the row, count +1 |

## 1. High-resolution timeouts

### The problem

The only clock interrupt is CPU 0's 100 Hz PIT; it broadcasts the 0x81
reschedule IPI to the other CPUs (`arch/x64/timer.c`; the AP LVT timers are
deliberately never armed periodic, see the comment in `timer_init`).  Every
park deadline was a `wake_ms` checked by `process_check_sleeping()` from
`schedule()`, i.e. at the next tick: a 1 ms sleep took 9-10 ms, a 100 us one
10 ms, and the deadlines were rounded to milliseconds on the way in, so with
the millisecond clocks some waits even returned early (base H1 `early=4`,
H5 `early=79`).  Under Wine, `Sleep(1)` is `select({0,1000})`
(`NtDelayExecution`), `SleepEx(.., TRUE)`, waits and waitable timers are
wineserver timeouts in `epoll_wait`, and C6 measured `Sleep(1)` at 8-22 ms.

### The design

* **Microsecond deadlines.**  `struct process` gained `wake_us`.  `wake_ms`
  stays the one field every park site and waker reads and clears (60-odd
  sites, including GPU and NFS files this lane does not own), and
  `proc_set_deadline_us()` writes both: `wake_ms` = the ceiling of `wake_us`
  in ms.  `proc_deadline_us()` honours `wake_us` only while `wake_ms` is
  exactly that ceiling, so a site that writes or clears `wake_ms` alone can
  never be overridden by a stale `wake_us`.  `process_check_sleeping_us()`
  compares in microseconds (`timer_get_us`, the TSC).
* **A per-CPU one-shot.**  Each CPU's LAPIC timer in one-shot mode on its
  own vector 0x84 (`hrt_arch_arm_us`, divide-by-16, the count from an exact
  calibration of the same 60 ms PIT window).  Never periodic, never vector
  32: the handler only preempts user mode, like the 0x81 IPI, so the
  first-instruction preemption the periodic AP LVT caused cannot happen.
  `hrt_armed_us[cpu]` is when each CPU's one-shot fires.  Invariant (under
  `proc_lock`): every pending deadline D has a CPU with
  `now < hrt_armed_us[c] <= D`, or a fire in flight.
  - `schedule()`, for a thread that parks with a deadline, covers it
    (`hrt_cover_locked`): nothing if another CPU is already armed at or
    before D, else this CPU arms (a lower deadline reprograms).
  - The fire (`sched_hrt_fire`): clear this CPU's armed time, expire every
    due sleeper, cover the nearest remaining deadline (sleepers and the
    Linux timer table), then run the timer table's tick.  The woken
    threads get a CPU: this one if it was idle (`schedule(tf,1)`), idle
    CPUs by directed IPI (`sched_kick_idle_cpus`), and when there are not
    enough idle CPUs and this CPU is in user mode, it is preempted.  The
    first woken slot is this CPU's next pick (`hrt_boost_idx`: the
    round-robin scan starts there), which is what keeps the error small
    under load.  A boost is only granted by a hardware fire, so a thread
    that sleeps 1 us in a loop cannot monopolise a CPU.
  - Spans above 1 s are approached in 1 s steps.  A fire that lands early
    (calibration) finds nothing due and re-arms for the rest.
  - The 100 Hz tick is unchanged and still checks every sleeper, so a CPU
    that sits with interrupts off past its one-shot only delays a wake to
    the next tick, as before.
* **The calls.**  nanosleep; clock_nanosleep (§2); futex WAIT /
  WAIT_BITSET (relative and absolute, monotonic or realtime) and LOCK_PI
  (absolute CLOCK_REALTIME, as Linux: it was compared with uptime); poll,
  ppoll, select, pselect6 (`file_poll_us`, `file_select_words_us`; the old
  ms entry points wrap them for native callers); epoll_wait, epoll_pwait,
  and `epoll_pwait2` (441, was ENOSYS); the timerfd read.  The re-scan
  slices of the poll engine (10 ms, 250 ms for all-notifying sets) are now
  microsecond deadlines too, capped by the caller's deadline.
* **The timer table** (itimer, POSIX timers, timerfd; `linux_compat_timers.c`)
  is in microseconds.  An armed timer is covered by a one-shot
  (`sched_hrt_cover_ext`) and the fire runs the table's tick on any CPU (the
  expiry is re-checked under `lt_lock`, so two CPUs cannot both fire it).
  timerfd expiries are also counted lazily from the clock at read, poll and
  gettime (`lt_catch_up_locked`), and a poll/select/epoll scan that sees an
  armed timerfd caps its park at the expiry (`fs_poll_cap_*`, per CPU).
  ABSTIME `it_value` is converted per clock (realtime clocks shifted by the
  realtime-monotonic offset; it used to be taken as monotonic ms).
* **Clocks.**  CLOCK_REALTIME / REALTIME_COARSE and gettimeofday now read
  microseconds (`hb_clock_realtime_us`; Wine's `NtQuerySystemTime`, which
  `NtDelayExecution` re-reads around each select).  `clock_getres` says
  1 us (10 ms for the COARSE clocks).

### Results (strix guest, `linux_probe_hrtimer`)

Error = actual - requested, microseconds, CLOCK_MONOTONIC, 200 samples per
row.  Grade: idle p50 <= 200 and p90 <= 1000; load (8 CPU-burning threads
plus the measuring one, 9 runnable on 8 vCPUs) p50 <= 500, p90 <= 2000; no
early return.

| row | base p50 / p90 / max | lane p50 / p90 / p99 / max | host p50 / p90 |
|---|---|---|---|
| H1 nanosleep 1000 us | 8995 / 9267 / 12304 (4 early) | 21 / 50 / 4068 / 5606 | 52 / 53 |
| H2 nanosleep 100 us | 9896 / 9940 / 10361 | 27 / 27 / 54 / 65 | 51 / 52 |
| H3 nanosleep 10 us | 9657 / 10001 / 15150 | 19 / 35 / 57 / 70 | 51 / 52 |
| H4 clock_nanosleep ABSTIME 1 ms period | EOPNOTSUPP | 22 / 46 / 1006 / 1843 | 10 / 53 |
| H5 futex timeout 1000 us | 8555 / 8997 (79 early) | 37 / 52 / 2292 / 2818 | 53 / 53 |
| H6 poll(0, 1 ms) | 8996 / 9010 | 37 / 57 / 2625 / 2797 | 53 / 53 |
| H7 ppoll 100 us | 9894 / 9933 | 30 / 36 / 75 / 257 | 51 / 55 |
| H8 select {0,1000} (Wine's Sleep(1)) | 8996 / 9026 | 37 / 50 / 1491 / 2110 | 53 / 56 |
| H9 epoll_wait 1 ms | 8997 / 9020 | 35 / 45 / 1022 / 2046 | 53 / 53 |
| H10 epoll_pwait2 200 us | ENOSYS | 30 / 34 / 163 / 310 | 51 / 52 |
| H11 timerfd 1 ms period, read | 4.5 s / 8.1 s (expiries counted at the tick only) | 37 / 47 / 460 / 841 | 3 / 4 |
| H12 poll(timerfd) 1 ms one-shot | 8996 / 9035 | 37 / 47 / 2730 / 3333 | 2 / 12 |
| L1 nanosleep 1000 us, load | 8997 / 8998 | 26 / 27 / 49 / 82 | 53 / 57 |
| L2 futex 1000 us, load | 8997 / 8998 | 22 / 26 / 2577 / 2742 | 52 / 58 |
| L3 epoll_wait 1 ms, load | 8997 / 9003 | 25 / 27 / 1593 / 1707 | 53 / 58 |

(The host's 50 us is Linux's default timer slack, not latency.)  The p99 of
1-5 ms is 2-3 % of samples in most rows; it is not the one-shot (the
median and p90 are tens of microseconds) and was not chased further: the
candidates are a CPU holding interrupts off (a long kernel row on the serial
console, C0-console's item) while it covers the deadline for others.

### 1.1 Idle cost (strix host CPU of the guest's QEMU process)

The one-shot adds no periodic interrupt: an idle CPU is armed only while
some thread sleeps with a deadline it covers, and one fire serves every
deadline due.  Measured with a strix-only probe (30 s in one nanosleep,
then 30 s of nanosleep(1 ms) in a loop; not committed) and `/proc/<qemu>/stat`
sampled each second, first 3 s of each phase skipped
(`k6-idle-{base,lane}.{txt,sum}`):

| phase | base | lane |
|---|---|---|
| idle guest (one thread in a 30 s sleep) | 7.7 % of one host CPU | 8.0 % |
| one thread in nanosleep(1 ms) for 30 s | 7.4 % (3112 sleeps, ~104/s: each took a tick) | 8.3 % (29096 sleeps, ~970/s) |

The idle cost is the 100 Hz PIT plus its 0x81 broadcast to 8 vCPUs, as
before.  A thousand 1 ms wakes a second cost about one percent of a host CPU.

**Under Wine** (`p8d3d12caps.exe` timing rows): §10 -- `50x Sleep(1)` is
51.4-51.9 ms (C6/C9 on base: 380-1210 ms).

## 2. clock_nanosleep

`l_core_clock_nanosleep` (linux_compat.c; the trap.c copy is the dead
fallback):

| clock | relative | TIMER_ABSTIME |
|---|---|---|
| REALTIME 0, TAI 11, REALTIME_ALARM 8 | monotonic + req | `abs - realtime_now` from now (TAI offset 0, as Linux until adjtimex sets one) |
| MONOTONIC 1, BOOTTIME 7, BOOTTIME_ALARM 9 | monotonic + req | `abs` (CLOCK_MONOTONIC is `timer_get_us` since b8cc46c8) |
| PROCESS_CPUTIME_ID 2, or this group's dynamic CPU clock | the group's CPU time + req, anchored once per call | the group's CPU time |
| MONOTONIC_RAW 4, REALTIME_COARSE 5, MONOTONIC_COARSE 6, THREAD_CPUTIME_ID 3 | -EOPNOTSUPP (no nsleep in Linux) | same |
| anything else | -EINVAL | same |

A past absolute deadline returns 0 at once.  The CPU clock parks in 1 ms
slices and re-runs (a rewound park, kind 2; its anchor is keyed by call site
and req pointer); `clock_nanosleep` is in the "never restarted" list for a
signal handler.  Every compat process is uid 0, so the alarm clocks need no
CAP_WAKE_ALARM (the host probe, unprivileged, gets EPERM and the row accepts
that).  `clock_gettime` answers TAI and the alarm clocks.  `rem` is still not
written for an interrupted relative sleep.

## 3. Signals to a group whose leader thread has exited

C8's P8 stop 7: after the wineserver died the game's main thread was gone
(THREAD_DONE) while its other threads blocked, and the launcher's
`kill(pid, SIGKILL)` returned ESRCH.

* `process_group_alive_locked()`: an anchor in THREAD_DONE with
  `live_threads > 0` is a live process.  `process_signal_directed()`
  resolves a kill(2) (not tkill/tgkill: that names the dead task) of such a
  pid or `-pid` to the group, `process_kill_group_sig()` and `process_kill()`
  accept it, `kill(pid, 0)` answers 0 (`process_pid_alive`).  A handled
  signal goes to the group's pending set and the member wake picks a live
  member even when the dead anchor's mask would take it.
* **set_tid_address** kept nothing.  musl's main thread passes
  `&__thread_list_lock`; a main thread that `pthread_exit()`s holds that
  lock and relies on the kernel's clear-child-tid at its exit to release it.
  So in a leader-exited group no other thread could exit, create a thread or
  `pthread_exit` again: killgrp K7 (the last worker returns) was never
  reaped, on base too.  The pointer is now the thread's `clear_child_tid`,
  zeroed and futex-woken by `process_thread_exit` (GB-021).

## 4. signalfd

`signalfd(282)` / `signalfd4(289)`: `FILE_TYPE_SIGNALFD` holding a mask.
`fd == -1` creates (SFD_CLOEXEC, SFD_NONBLOCK), otherwise the signalfd's
mask is replaced (EINVAL for another kind of fd); sizemask must be 8;
SIGKILL/SIGSTOP are dropped from the mask.  A read dequeues the READER's
pending signals in the mask (thread-directed first, then its own, then the
group's), lowest first, 128-byte `signalfd_siginfo` records: `ssi_code`
SI_TKILL for a directed signal, SI_USER otherwise; SIGCHLD carries
CLD_EXITED/CLD_KILLED, the child pid and status (`sig_child_*`).  The sender
pid is not recorded (0).  Poll/epoll report it readable from the polling
thread's pending sets, and it is a notifying fd: a signal sent to a
Linux-ABI process calls `poll_notify()`, only while a signalfd exists.

Linux queues a **blocked** signal even when its disposition is SIG_DFL
(`sig_ignored()` is false for a blocked signal), and complete_signal()
considers it blocked when every thread blocks it.  Here a SIG_DFL signal
killed the group at once (terminate) or was dropped (SIGCHLD and the other
default-ignored ones), so `sigprocmask(SIG_BLOCK, {SIGTERM}); signalfd(...)`
was a kill.  Such a signal now takes the pending path; the delivery engine
applies the default action if it is ever unblocked, and a default-ignored one
no longer counts as deliverable for EINTR.

## 5. ru_stime

`general_interrupt_handler` stamps `ru_kin_us` on every trap from user mode
and adds the time to `ru_ksl_us` when it returns to that user frame;
`ru_charge_locked` (schedule) charges the slice's kernel time -- closed round
trips plus one still open (a thread parking in a syscall) -- to `ru_sys_us`
and the group's `ru_grp_sys_us`, and reaped children's into `ru_ch_sys_us`.
getrusage and wait4 report utime = total - stime.  Two `timer_get_us` per
trap.  Interrupt time while in user mode (the tick) counts as system time,
as it is on Linux with tick-based accounting.  x86_64 only (ARM: stime 0).

## 6. fd installs under fdtab_lock

`fd_install_cloexec()` scans, installs and sets/clears FD_CLOEXEC under
`fdtab_lock`.  eventfd, timerfd_create, epoll_create(1) (EPOLL_CLOEXEC is
honoured now), memfd_create, signalfd and both accept paths (AF_INET
loopback, AF_UNIX) use it; they allocate the file first and release it on
EMFILE.  `file_connect` already used `fd_install`.  Base fdrace D1: 1764
duplicate fd numbers and 616 fds answering the wrong type in 19200 creates.

## 7. [FRAME-DFREE]

`frame_free()` of a frame whose bit is already clear is still ignored, and
now counted (`frame_dfree_count`) and printed:
`[FRAME-DFREE] n= phys= caller=` (the first 16, then every 1024th; raw
`uart_putc_dbg`, the [FREEW] printer).  The unit suite's idempotence case
produces exactly one row.

## 8. Gates

See §11.

## 9. A consequence: the kernel-mode #PF "repeat" guard (5c8a1e63)

The demand-fault path for kernel accesses to user memory kills the process
when one page faults four times in a row on a CPU (a resolution that does
not stick).  It counted every fault on that page, however far apart.
`linux_probe_e2fault` preads into a 64 KiB window another thread
MAP_FIXED-replaces after `usleep(200..1100)`; with the one-shot those sleeps
now last 200-1100 us instead of a tick, the window's first page faulted once
per replacement, and the 4th killed the reader with SIGSEGV in `memcpy`
(pfx_gate FAIL 3/3 on a06a49b5; a diagnostic row named the guard,
`why=REPEAT rep=4`).  A repeat now has to come within 100 us of the previous
fault on that page: e2fault 3/3 PASS, 560280 preads and 41551 remaps in
60 s (`e2fault-repeat-guard.txt`).  Wine maps with MAP_FIXED from many
threads; the old guard was a latent SIGSEGV for the game too.

## 10. proc_lock off the hot paths (integrator item 0, C9-play's P9-2)

C9's profile (`docs/cp-p9.md` §5): in the world the guest was 53-65 %
kernel, `spin_acquire_raw` 28-32 % of all samples on `proc_lock`,
`get_cpuid` 6-7 %.  An fpsprobe boot on this lane's kernel (before) showed
the same (`fpsprobe-before1.txt`), and its serial said who held the lock: the
`[LOCKDIAG]` holders (owner_site2) were `start_scheduler`'s idle loop (35 of
50) and `wd_dump_proc_table` (14), and the console carried 1232
`[IDLESTUCK]` table dumps and 19140 `[ALIVE-SET]` rows, every one printed
with proc_lock held (one outb VM exit per byte).

| change | where | why |
|---|---|---|
| idle loop looks before it locks | `start_scheduler` | an idle CPU woke on every interrupt and took proc_lock to scan the table twice; now a lock-free look (no READY slot, no sleeper due, the 10 ms full scan not due) sends it back to `safe_wfi`; every 64th pass takes the slow path |
| the lost-owner reaper at most every 50 ms | `start_scheduler` | `sched_idle_rounds` is reset only by a pick in the idle loop, so in a busy machine (picks go through `schedule()`) the reaper's table scan ran on every idle pass once past 500 |
| `[IDLESTUCK]` and `[ALIVE-SET]` dumps only after 2 s with no pick anywhere | `start_scheduler`, `schedule` | they exist for a machine that has stopped; in the game they fired every few seconds with proc_lock held |
| sleeper scan only when due | `schedule` | `sleep_next_us` (lowered by every covered park) and a 10 ms backstop; the old code scanned 512 slots on every `schedule()` |
| no duplicate kick | `sched_wake_idle_cpu`, `sched_kick_idle_cpus` | `sched_kick_pending[cpu]`, cleared at `schedule()` entry; the search starts after this CPU (it always picked the lowest idle CPU) |
| watchdog dump without the lock | `wd_dump_proc_table` | a stall dump to COM2; racy reads are fine |
| flusher liveness lock-free | `process_others_alive` | "alive" from a racy read is safe; only "nothing else alive" is confirmed under the lock |
| one-load `get_cpuid` | `cpu.c`, `smp.c`, `trap.c` | once every CPU the boot started has set GS_BASE (a per-CPU bit at `trap_init_core_with_id`; `smp_init` says how many), ring-0 GS is always that CPU's block: `mov %gs:32` instead of `rdmsr` |

Not changed: `rt_sigprocmask` takes no proc_lock (C9's stack words there
were stale; its trap exit has a lock-free fast path), and select/poll still
park under proc_lock once per park (`fs_poll_park`), as `poll_notify()` does
once per readiness event with waiters.  `schedule()` itself still runs under
proc_lock: a per-CPU run queue is the next step if the remaining 9.6 % spin
matters (§11 lists what is left).

Results (strix, Venus RX 9070 XT, `-smp 8 -m 12G`, `fpsprobe.p9`; world =
the 3-minute walk, prof = the 20 s sampled window):

| run | world median fps | world p10 | menu median | kernel % | spin_acquire_raw % | get_cpuid % | idle % | user % |
|---|---|---|---|---|---|---|---|---|
| C9 gateA4 (integ7 kernel, 32 GiB) | 13.8 | 7.2 | 32.9-40.6 | 56-65 | 28-32 | 6-7 | 27-33 | 8-11 |
| before1 (this lane before §10) | 12.3 | 6.5 | 44.9 | 55.5 | 28.0 | 5.8 | 35.0 | 9.5 |
| before3 | 10.2 | 6.8 | 41.2 | 52.9 | 23.2 | 7.9 | 34.8 | 12.4 |
| after1 | **18.8** | 10.8 | **89.5** | **31.7** | **9.6** | **0.3** | 53.0 | 15.3 |
| after2 | **18.7** | 11.7 | 70.0 | 31.8 | 8.9 | - | 52.9 | 15.3 |
| after3 (+ a 30 s stack sample) | 17.4 | 12.2 | 80.3 | 32.4 | 7.2 | 0.7 | 52.0 | 15.6 |

(before2 lost the LOAD GAME list focus in the harness before the world:
`fpsprobe-before2-harness-miss.txt`.)  The world load (menu to "in world")
took 116 s before and 81 s after.

What is left (after3's stack words, `fpsprobe-after3-stack.txt`): the
remaining ~8 % spin is `schedule()`'s pick, `fs_poll_park` and the futex
wait/wake, all still under proc_lock, plus `vm_touch` (vm_lock).  The guest
is now half idle in the world: the next limit is not lock contention alone
(Venus's synchronous software-WSI present is C6-p7's).  A per-CPU run queue
and per-waiter futex/poll wakeups are the structural next steps.
The game's own code went from 6.7 % to 9.3 % of samples.

Under Wine (`p8caps`, the same boots): `50x Sleep(1)` = 51.4-51.9 ms
(base/C9: 380-1210 ms), `SleepEx(50)` max 50.07 ms (base max 60), waitable
timers 50.3-51.8 ms.

## 11. Gates (strix; parity reference: integ7, `strix:~/cp/integ-gates10.txt`)

Two full batches (`strix:~/cp/lanes/k6-gates.sh`, a copy of
`integ-gates10.sh` pointed at this tree; P8 lavapipe and the separate Venus
P6 boot left out): run 1 on a06a49b5, run 2 on 0352bd42 (everything but the
clock-probe fix).  `gates-run1.txt`, `gates-run2.txt`.

| gate | integ7 | run 1 (a06a49b5) | run 2 (0352bd42) |
|---|---|---|---|
| build | ok | ok | ok |
| 45 linux probes (+5 new) | all pass | all 50 pass | 48/50: clock (§11.1), pemap V1 once (§11.2) |
| ext2_gate, exec_gate, rsize_guard, rsize_scan, castscan, ARM build | pass | pass | pass |
| unit | 2 known reds (virtio_gpu_test.c:214/:244) | the same 2 | the same 2 |
| host tests | 38/40 | 38/40 | 38/40 |
| pfx_gate | pass | **FAIL** (e2fault: §9) | pass |
| ext2w_gate | FAIL (leak-class pull-the-plug) | pass | pass |
| reg_gate, ext2_test_host | pass | pass | pass |
| exitgroup x3 | 3/3 | 0/3 (X7 1-2 bad, ~1.05-1.24 s) | 3/3 |
| Gate P2 | pass | pass | pass |
| Gate P4 G1-G3 | pass | pass | pass |
| Gate P5 vkcube/d3d11/d3d12/fullscr | graded FAIL (console interleaving) | 4/4 PASS | 4/4 PASS |
| thrstorm (256 Wine threads, 60 s) | pass, 261 rounds | pass, 263 rounds | pass, **760 rounds** |
| k5probes (K5_GRADE_KILL=1: p8killprobe K1/K2 graded) | pass, K1/K2 FAIL (not graded) | pass incl. K1/K2 | pass incl. K1/K2 |
| P8 on Venus | menu 2 of 3 at launch + 161-177 s | the clean early exit at +20 s (P9-1, game status 0) | **main menu at launch + 109 s**; +fps median 80.6 (integ7 boots: 46.6, 48.2) |

exitgroup X7 is the known load flake (integ7 notes): on base 058fd3d5 at the
same time it failed 2 of 3 (`[LP] FAIL X7`), and run 2 passed 3/3.

### 11.1 clock

`linux_probe_clock`'s `sem_pingpong` measures 200 semaphore round trips with
a millisecond clock and failed when they took 0 ms (`per_us=-1`).  They now
do; 07b62408 counts 0 ms as per_us 0 (`run2-clock-pemap.txt`).

### 11.2 pemap

V1 (COW of a memfd page across threads) read the old value for 6267 more
reads in 2 of 12 iterations, once, in run 2 with four guests on strix.  Then
HEAD passed 6/6 and the previous kernel 4/4 (`run2-clock-pemap.txt`).  Not
reproduced; C1-pe's own note says the cross-CPU staleness is not proven gone.

### 11.3 The game

P8 Venus run 2: `p8-venus-run2.txt` (menu OCR, +fps rows vs the integ7 boots,
p8caps: 50x Sleep(1) = 51.4-51.6 ms).  thrstorm's 2.9x more Wine round trips
per minute is the wineserver IPC path getting the §1 and §10 wins.

Five fpsprobe boots (P9 harness, Venus, 12 GiB) on this lane's kernels went
to the main menu (+fps rows: menu median 41-90 fps) and four loaded the
user's save into the world (the fifth was a harness menu-navigation miss);
§10 has the numbers.

## 12. Round 2: the wake path (integrator round-2 items)

Starting point: integ9 (adb81ee4: cyberpunk with C0-console, cp-p7, cp-play,
cp-k6, cp-fork).  An fpsprobe on integ9 as it is: world median 17.5 fps,
kernel 33.8 %, `spin_acquire_raw` 9.6 %, idle 51.5 % (`r2-fpsprobe-integ9.txt`).

The new probe `linux_probe_ipclat` times the wineserver round trip (a client
writes its request pipe and blocks reading its reply pipe; a single-threaded
server sleeps in epoll_wait over every client's request pipe): on integ9
24-29 us per round trip (the strix host: 3 us), 29 us with 256 fds in the
set; one trivial syscall costs 154-191 ns, so the time was the wake, the
switch and the rescans, not syscall entry.

| change | where | effect |
|---|---|---|
| **sync wake**: a wake inside a user thread's syscall (pipe/AF_UNIX/eventfd write, futex wake, poll notify of one waiter) defers its IPI; if the waker parks within its next 30 us (its next syscall: Wine writes the request, then reads the reply) the woken thread is that CPU's next pick (no IPI, no HLT exit on another vCPU); otherwise the syscall exit or this CPU's one-shot at the end of the window sends the IPI | `process.c` `sched_wake_sync`, `sched_sync_flush_ex`; `trap.c` trap nesting (`sched_trap_depth/sys`) | ipclat 1 fd: 26 -> 4-6 us |
| **READY hint**: a 512-bit "maybe READY" mask set by every READY transition (`proc_mark_ready`, all 31 sites), read by the pick, the idle loop and the idle fast check instead of 512 PCB state fields (~11 KiB apart: 512 cache misses under proc_lock per pick); stale bits are cleared at the pick, and a full scan every 10 ms repairs a missed one | `process.h`, `process.c`, the READY sites in `linux_compat_thr.c` and both `trap.c` | pick cost O(8 words + READY slots) |
| **lock-free pipe readiness probe** (the poll protocol already covers the race: count is updated under the pipe lock and THEN the sequence moves) | `fs.c` `f1_probe_fd` | 256-fd epoll scan 25 -> ~12 us |
| **keyed notifications**: `poll_notify_obj(pipe or eventfd)` sets one of 63 key bits (`poll_key_bit`); a parked poller carries the filter of the keys its scan saw (`poll_bloom`, bit 63 = "has a keyless fd": AF_UNIX, signalfd) and only matching pollers are woken; keyless `poll_notify()` still wakes everyone | `process.c` `poll_notify_bits`, `fs.c`, `pipe.c`, `linux_compat_fd.c` | no thundering herd for pipe events |
| **event-driven epoll**: each entry keeps its object's key; a keyed notification marks the sets whose filter has it; the next wait rechecks only those entries, the keyless and non-notifying ones and the ones it reported last time (level-triggered); full scans after epoll_ctl, keyless notifications, a full maxevents, every 64th wait, and with more than one waiter | `linux_compat_timers.c` | ipclat 256 fds: 29 -> 6 us |
| syscall table walk cached per number; the stack-parity detector hashed (8 probes, was a 512-entry linear search per syscall) | `linux_compat.c` | a few hundred ns per syscall |
| ICR pair written with interrupts off (an IRQ-path send between the two writes could redirect an IPI) | `process.c` `resched_cpu` | correctness |
| `rem` written for a relative nanosleep/clock_nanosleep ended by a signal (at the resume, in the thread's own AS); signalfd's `ssi_pid` = the sender's tgid | `linux_compat.c`, `linux_compat_thr.c`, `process.c`, `linux_compat_fd.c` | hrtimer C10, sigfd S12 |
| `rt_sigtimedwait` takes a signal of its set that is pending (it was an `-EAGAIN` stub; blocked SIG_DFL signals are queued since §4); nothing pending is still `-EAGAIN` at once (no blocking wait, the stub's Mono note) | `linux_compat_proc.c`, `linux_compat_fd.c` `l_sig_dequeue` | sigfd S10's cleanup |

Not done: a real per-CPU run queue (the pick still runs under proc_lock; the
hint removes its scan, not its lock), per-object wait lists for AF_UNIX
(keyless), and futex hash buckets (the futex wake scans `l_thr[]`, 32 KiB
of contiguous 64-byte slots, which the profile does not show).
`vm_touch` is no longer in the top of the profile.

### 12.1 Round-2 results (strix)

`linux_probe_ipclat` (guest; host: 3 us everywhere):

| row | integ9 | round 2 |
|---|---|---|
| I0 syscall floor (getppid / pipe write+read on one thread) | 191 / 680 ns | 157-181 / 464-619 ns |
| I1 epoll server, 1 fd, round trip p50 | 26 us | **4 us** |
| I2 64 fds | 24 us | **5 us** |
| I3 256 fds | 27 us | **6 us** |
| I4 poll server, 256 fds | 35 us | 17 us |
| I5 4 clients against 256 fds | 64 us, 59.6k/s | 63 us, 51.7k/s |

(I5: four clients serialise on the one server; the sync wake keeps the
server on the waker's CPU, so this row does not improve.)

fpsprobe (Venus, `-smp 8 -m 12G`, the default Venus ICD `~/cp/venus-build`
for both kernels -- not cp-p7's rebuilt Mesa):

| run | world median | world p10 | menu median | kernel % | spin % | idle % | user % |
|---|---|---|---|---|---|---|---|
| integ9-a | 17.5 | 9.2 | 94.1 | 33.8 | 9.6 | 51.5 | 14.7 |
| integ9-b | 20.4 | 11.4 | 73.9 | 31.7 | 7.9 | 52.6 | 15.7 |
| round 2 a1 | **24.0** | 15.9 | 125.2 | **22.8** | **4.8** | 60.3 | 16.9 |
| round 2 a2 | **21.0** | 15.0 | 94.6 | **17.0** | **2.7** | 65.9 | 17.1 |

The fps medians move with strix's load (other lanes' guests share it); the
profile does not: the kernel share is down by a third to a half and the spin
by two thirds, and the guest is now 60-66 % idle in the world.  What spins
now (a2's stack words, `r2-fpsprobe-r2-a2-stack.txt`) is the Venus fence
path (`virtio_gpu_3d_fence_kick_check`, `gpu_m2_fence_wait2`; C6-p7's
files) and futex.  p8caps in these boots: 50x Sleep(1) = 51.3-52.1 ms.

## 13. Kernel-task stack sharing (integrator top priority; C6-p7b's fix adopted)

The race C3-ext2w proved: a kernel task runs in ring 0 on its own stack, so
the interrupt that preempts it (a yield's int 0x81, the 100 Hz broadcast,
a blk-cache wait) runs schedule() on THAT stack.  schedule() saved the
task, made it READY, claimed the next slot and dropped proc_lock, then ran
the switch tail (resume diagnostics, vm_touch, the next thread's signal
delivery, enter_user_space's frame check) still on the task's stack; another
CPU could pick the task in that window and resume it at its saved RSP, onto
the frames the first CPU was still using.  integ9's P5 fullscreen crash is
this: CPU 6 and CPU 7 both faulting in spin_acquire_raw(&proc_lock) on one
stack (same RSP on both), then [UNOWNED-FREED] and [LOSTWAKE].

C1-k6 wrote a fix of its own first (defer the READY publication until the
CPU is off the stack; kept on branch `cp-k6-kstack-defer`, not proposed);
the integrator asked for C6-p7b's instead, which covers the whole tail with
one rule and has the stronger probe:

* ce37286a (C6-p7b 5564bd40) -- detector: `cpu_locals.kstack_pin` (gs:[56])
  names the kernel task whose stack this CPU is still on, set under
  proc_lock when schedule() gives a kernel task up, cleared by
  enter_user_space just before the iretq and by start_scheduler after the
  nothing-READY longjmp; both pick sites report a pinned pick as
  [KSTACK-SHARE].  Probe `linux_probe_kstack` (6 kernel tasks yielding in a
  loop for 4 s against 4 churning user processes, 2 rounds, canaries in each
  task's frame) through the test-only syscall 0x4B5354 (-ENOSYS on release
  kernels) and its console checker `tools/gate/lp_check_kstack.py` (FAIL on
  any [KSTACK-SHARE], kernel fault or lock-strand row).
* 63733ef8 (C6-p7b a0235c57) -- fix: both pick sites skip a kernel task
  whose pin another CPU holds (the task stays READY; it is picked a few us
  later).  In this tree every pick goes through those two loops: round 2's
  READY hint and the one-shot's boost (`hrt_boost_idx`) only choose where
  the schedule() loop starts and which slots it examines, and the sync wake
  picks through the same loop, so the pin check covers them.  User tasks
  have no such window: their kernel frames are on the per-CPU stack and a
  user resume is claimed before proc_lock drops.

Gate row (strix): `kstack`

| tree | result |
|---|---|
| round 2 + detector (ce37286a), run 1 | FAIL: wedged after S1 (probe never finished) |
| round 2 + detector (ce37286a), run 2 | FAIL: [KSTACK-SHARE] n >= 4096 within the first round, `FATAL: Exception in Kernel Mode`, [STALLBREAK] on a lock whose owner CPU's stack was shared |
| round 2 + fix (63733ef8), gate batch | PASS: yields 31127 + 28201, share=0, canary_bad=0 (lp_check_kstack PASS) |

Gate batch on 63733ef8 (strix, `docs/cp-k6-evidence/r2-gates-63733ef8.txt`):
every t3 probe PASS (54 incl. kstack and console), ext2_gate, exec_gate,
rsize_guard, nfs_proto, host 38/40 + kconsole, castscan 0 new, ARM build,
rsize_scan, panic_sink, pfx_gate, reg_gate, ext2_test_host, exitgroup x3,
P2 G1-G3, P4, P5 (vkcube, d3d11, d3d12, fullscr: the row integ9 crashed in)
all PASS; unit 462 run / 2 known reds (virtio_gpu_test 214, 244), as integ9;
ext2w_gate FAIL (1) (crash1: e2fsck rc=4, block-bitmap leak after the QMP
quit) -- integ9's own batch fails the same way (crash7), and so did the
round-2 batch on 5ae97d4d (crash2, plus a harness QMP-socket miss on crash8).

(C6-p7b's own numbers on integ9 + its series: detector FAIL with 16
SHARE rows and a kernel #PF; fix PASS, 65847 yields, share=0,
pin_skips=7829.)

## 14. A halt on behalf of a task wakes by itself (C3-fat's P9-8, systemic half)

C3-fat found P9-8 is a lost wakeup: blk_token_acquire waits in safe_wfi()
(`sti; hlt`) and blk_token_release wakes nobody, so the waiter looks again
only at its CPU's next interrupt -- and an AP's only periodic interrupt is
CPU 0's broadcast 0x81.  CPU 0 inside a syscall runs with IF=0 (SFMASK
0x47700), so when it spins there (on a ylock the halted CPU holds) the tick
stops for every AP and the holder never wakes.  C3-fat fixes its own NVMe
token wait; the systemic part is here.

Fix: `safe_wfi()` (x86_64) now does `cli; sched_wfi_bound(); sti; hlt`.
`sched_wfi_bound()` (process.c), when this CPU claims a slot (a kernel task
or a syscall waiting on a device, not the idle loop, which is woken by the
IPIs that hand it work), makes sure this CPU's own LAPIC one-shot (round 1's
vector 0x84) fires within 1 ms: an earlier armed deadline is kept, a later
one is pulled in and re-covered by the fire's rescan.  IF=0 from the arm to
the hlt, so the one-shot cannot fire before the CPU halts.  Every safe_wfi
wait site (virtio_blk 7, net 5, process.c's claimed halts, ext2_vfs,
lockscale, virtio_gpu) is bounded without touching them; the cost is one
LAPIC write per halt and a 0x84 fire per 1 ms of halting.  The ylock spin
itself is unchanged (enabling IF inside it is only safe for callers that
allow it, which the lock cannot know).

Probe `linux_probe_wfibound` (test syscall 0x4845, test kernels only):
W1 = longest of 200 halts from a syscall (limit 2500 us); W2 = the P9-8
shape: a thread halts in the kernel for a flag that no wakeup announces, a
second thread on CPU 0 sets it with IF=0 and spins until the waiter saw it
(limit 20 ms; the spinner gives up at 500 ms).

| tree (strix, run 1 / run 2) | W1 longest halt | W2 waiter latency |
|---|---|---|
| bound off (local experiment: the call removed) | FAIL 10647 / 10095 us | FAIL 500107, 500099, 500100 / 500099, 500107 us (and one 20 us: another interrupt) |
| bound on (first build) | PASS 1045 / 1036 us | PASS 996, 1012 / 1013, 1033 us |
| bound on (9d47b051, the committed probe) | PASS 1038 / 1052 us | PASS 979, 1032, 1005 / 990, 1006, 1000 us |

The 500 ms rows are the wedge: the waiter saw the flag only when CPU 0 gave
up and its tick came back.

Evidence: `docs/cp-k6-evidence/r2-kstack.txt`, `docs/cp-k6-evidence/r2-wfibound.txt`.
