# cp-ntsync — `/dev/ntsync` for Wine's synchronisation (lane C2-ntsync, wave 6)

Plan: `docs/cyberpunk.md` §7.2f (C2-ntsync).  Branch `cp-ntsync` from integ11
@ 13dc693b.  Evidence: `docs/cp-ntsync-evidence/`.  Every number is from
strix (Zen 5, `ssh strix`); guests are intel `MODE=test`, KVM `-cpu host`,
8 vCPU, 12 GiB (P9 game boots 32 GiB under `/tmp/cp-bigguest.lock`).

**Result.**  `/dev/ntsync` is in the compat kernel
(`src/kernel/linux_compat_ntsync.c`, Linux 6.14 UAPI and semantics, 116 probe
rows), Wine 11.0 rebuilt with its own ntsync support switched on
(`~/cp/lanes/ntsync-wine`, two files differ from `~/cp/wine-build-x`) uses it
in the guest, and falls back cleanly where the device is missing.  In the
world (fpsprobe, Venus, scanout, 3 interleaved boots each, same kernel):

| | ntsync Wine | stock Wine (no ntsync) |
|---|---|---|
| world fps median (p10), per boot | 26.8 (23.5), 24.9 (22.8), 26.6 (20.9) | 22.7 (17.9), 20.5 (17.5), 28.2 (14.9) |
| mean of the medians / of the p10s | **26.1 / 22.4** | 23.8 / 16.8 |
| wineserver, share of one vCPU (kernel part) | **0.07-0.08** (0.06-0.07) | 0.29-0.30 (0.26-0.27) |
| the game's two hottest threads: running / in the kernel, % of wall time | **92-94 / 13-17** | 81-87 / 15-21 |
| LOAD GAME -> in the world | **89.5-92.0 s** | 100.4-107.1 s |
| launch -> main menu | 128-145 s | 143-145 s |

The wineserver leaves the game's waits (0.3 -> 0.07 vCPU), every ntsync boot's
p10 is above every stock boot's (20.9-23.5 vs 14.9-17.9), and loads are
~12 % faster.  The median moves +2.3 fps on average, inside the ±5 fps
boot-to-boot spread: **not** a reliable 30.  With ntsync the game's two
hottest threads are on a vCPU 92-94 % of the time and only 13-17 % of it is
kernel: the frame is now the game's own user-mode work on two threads.

Gates (lane at 15fc4b93 + HEAD reruns, every Wine gate on the ntsync stage):
at parity with integ11 (§6).

## 1. What Wine 11.0 already has (verified in the source of `~/cp/wine-build-x`)

`strix:~/cp/wine-build-x/build-x86_64/config.log`: configured from
`~/cp/wine-build-x/src/wine-11.0` (Wine 11.0), and

```
configure:9968: checking for linux/ntsync.h
conftest.c:62:10: fatal error: 'linux/ntsync.h' file not found
ac_cv_header_linux_ntsync_h=no          ->  /* #undef HAVE_LINUX_NTSYNC_H */
```

The glibc 2.31 sysroot predates the header (Linux 6.14).  Wine 11.0's ntsync
support is complete and gated only by that macro (`#ifdef NTSYNC_IOC_EVENT_READ`
after `#include <linux/ntsync.h>`):

* **server** (`server/inproc_sync.c`): `get_inproc_device_fd()` opens
  `/dev/ntsync` (`O_CLOEXEC | O_RDONLY`) once; when that succeeds every
  event (`event.c`), mutex (`mutex.c`), semaphore (`semaphore.c`) and every
  object's internal sync (`create_internal_sync`: processes, threads, message
  queues, files, ...) is an ntsync object created on it, and each thread gets
  an internal "alert" event (`thread.c`).  The server keeps one fd per
  object, sets/resets internal syncs with `EVENT_SET`/`EVENT_RESET`, and on a
  thread's death calls `MUTEX_KILL` with its tid on every mutex
  (`abandon_inproc_mutexes`).  `init_first_thread` sends the device fd to the
  client (SCM_RIGHTS); `get_inproc_sync_fd` sends an object's fd for a handle;
  `get_inproc_alert_fd` the thread's alert event.
* **client** (`dlls/ntdll/unix/sync.c`): `inproc_device_fd` is that fd (-1
  without it).  `NtWaitForMultipleObjects`, `NtSignalAndWaitForSingleObject`,
  `NtSetEvent/ResetEvent/PulseEvent`, `NtReleaseSemaphore`, `NtReleaseMutant`
  and the `Query*` calls try `inproc_*` first; `STATUS_NOT_IMPLEMENTED` (no
  device, or an object without an ntsync fd) falls back to the server.  Fds
  are cached per handle.  `linux_wait_objs()`:
  `WAIT_ANY` unless `WaitAll` with more than one object; timeout `~0` =
  infinite, a relative NT timeout becomes an absolute `CLOCK_MONOTONIC` ns
  value, an absolute one an absolute `CLOCK_REALTIME` value with
  `NTSYNC_WAIT_REALTIME`; `owner = GetCurrentThreadId()`; `alert` = the
  thread's alert event fd for an alertable wait, else 0; retries on `EINTR`;
  `index == count` means the alert (then a zero-timeout server wait runs the
  user APCs); `EOWNERDEAD` -> `STATUS_ABANDONED + index`; `ETIMEDOUT` ->
  `STATUS_TIMEOUT`.  `EOVERFLOW` from `SEM_RELEASE` / `MUTEX_UNLOCK` is
  `STATUS_SEMAPHORE_LIMIT_EXCEEDED` / `STATUS_MUTANT_LIMIT_EXCEEDED`, `EPERM`
  from `MUTEX_UNLOCK` `STATUS_MUTANT_NOT_OWNED`, `EOWNERDEAD` from
  `MUTEX_READ` the abandoned state.

There is no environment switch: the device's presence is the switch, so the
fallback is "open fails".

Runtime UAPI (`include/uapi/linux/ntsync.h`, Linux 6.14; strix's
`/usr/include/linux/ntsync.h`, sha256 `006437ee…6262`), restated in
`src/include/ntsync.h` with static asserts and checked against the real
header by the probe's host build:

| ioctl | nr | on | arg |
|---|---|---|---|
| `CREATE_SEM` | `_IOW('N',0x80)` | device | `{u32 count, max}` -> returns the new fd |
| `SEM_RELEASE` | `_IOWR('N',0x81)` | sem | `u32` in: count to add; out: previous count |
| `WAIT_ANY` / `WAIT_ALL` | `_IOWR('N',0x82/0x83)` | device | `{u64 timeout, u64 objs; u32 count, index, flags, owner, alert, pad}` (40 B; `index` at byte 20) |
| `CREATE_MUTEX` | `_IOW('N',0x84)` | device | `{u32 owner, count}` |
| `MUTEX_UNLOCK` | `_IOWR('N',0x85)` | mutex | `{owner, count}`; out: previous count |
| `MUTEX_KILL` | `_IOW('N',0x86)` | mutex | `u32` owner |
| `CREATE_EVENT` | `_IOW('N',0x87)` | device | `{u32 manual, signaled}` |
| `EVENT_SET/RESET/PULSE` | `_IOR('N',0x88/0x89/0x8a)` | event | out: previous state |
| `SEM_READ/MUTEX_READ/EVENT_READ` | `_IOR('N',0x8b/0x8c/0x8d)` | object | the args struct; `MUTEX_READ` answers `EOWNERDEAD` for an abandoned mutex |

## 2. The driver (`src/kernel/linux_compat_ntsync.c`)

Files: the driver and `src/include/ntsync.h` are new; the hooks are small
(C1-k7 owns `process.c` / `trap.c`, which this lane does not touch):
`src/include/fs.h` (`FILE_TYPE_NTSYNC`, `f->ntsync.obj`), `src/kernel/fs.c`
(`/dev/ntsync` open, the two release sites, fstat, three helpers),
`src/kernel/linux_compat_fd.c` (one line in the ioctl route),
`src/kernel/linux_compat_thr.c` (one `if` in `l_rewound_restartable`).  It
builds for ARM too (the compat class is x86_64-only at run time).

### 2.1 Objects and fds

`open("/dev/ntsync")` (fs.c, next to `/dev/null`) makes a `FILE_TYPE_NTSYNC`
slot holding a device object.  `CREATE_*` makes an object (it holds a
reference on its device) and installs it as a new `FILE_TYPE_NTSYNC` fd,
`O_RDWR | O_CLOEXEC` like Linux's `anon_inode_getfile`.  The global file slot
is what dup/fork/SCM_RIGHTS share (`ref_count`), so the server's fd and every
client's fd are one slot and one object reference; the last close releases
it (both release sites in fs.c).  A queued wait entry holds another
reference, so closing an fd under a waiter is safe (probe W11), as on Linux.
Objects live in frame-pool pages that are never returned (type-stable), one
spinlock `ns_lock` guards every object and queue.

### 2.2 Semantics (Linux's, case by case)

`is_signaled`: sem count > 0; mutex unowned or owned by the waiter's
`owner`, and count < `UINT32_MAX`; event signaled.  A waker acquires on the
waiter's behalf, exactly Linux's `try_wake_*`: every object keeps a FIFO of
`WAIT_ANY` entries (alert entries included) and one of `WAIT_ALL` entries;
a state change runs the `WAIT_ALL` waiters first (all objects at once or
nothing), then hands the object to `WAIT_ANY` waiters in queue order while
it stays signaled.  A new wait queues itself, then runs the same try-wake
on each of its objects in array order (which can satisfy an older waiter
first, as on Linux).  `EVENT_PULSE` = set, wake, reset in one critical
section, so exactly the queued waiters see it.  `MUTEX_KILL` abandons
(owner 0, count 0, ownerdead); the next acquirer gets `EOWNERDEAD` with the
index and owns it.  Validation as Linux: `count > max`, `!owner != !count`,
`pad`, unknown flags, more than 64 objects, duplicate objects in `WAIT_ALL`
(the alert included), an fd that is not an object of this device -> `EINVAL`;
a request the fd's kind does not have -> `ENOTTY`; bad user memory ->
`EFAULT`.

### 2.3 Waiting without sleeping in the kernel

The compat kernel has no in-syscall sleep: a blocking call parks (BLOCKED,
optional deadline) and re-executes when woken (the `-2` / `l_park`
convention).  So a wait's queue record (`struct ns_q`, one 4 KiB frame per
thread slot, allocated on first use) outlives the park:

1. first execution: copy and validate the args, resolve the fds, queue the
   65 possible entries, run the initial try-wake;
2. every execution, under `proc_lock` (the lock a signal sender holds, so a
   signal cannot slip between the look and the park): granted -> tear down,
   write `index`, return 0 (or `EOWNERDEAD`); a deliverable signal -> tear
   down, `EINTR`; the deadline passed -> tear down, `ETIMEDOUT`; else park
   with the deadline (`proc_set_deadline_us`, C1-k6's per-CPU one-shot meets
   it: W8/W9 measured 99.99 ms for 100 ms) and re-execute on wake.  A
   granted wait does not need `proc_lock` at all (the common case).
3. A re-execution is recognised by (slot generation, request, args pointer)
   on a queued record; nothing else can leave a record queued while the
   thread runs, because every other exit tears it down.

The wakers wake with `process_wakeup()` — C1-k6's sync wake (the woken thread
is the waker's CPU's next pick if the waker parks within 30 us, else an IPI),
one per granted waiter, collected under `ns_lock` and issued after it.

Signals: Linux answers `-ERESTARTSYS`, i.e. the wait restarts transparently
under `SA_RESTART` and is `EINTR` otherwise.  Here a handled signal always
ends a parked wait with `EINTR` (a one-line hook in `linux_compat_thr.c`
`l_rewound_restartable`: the queue entries must be gone before a handler
runs, or a wait inside the handler would find the outer one's record).
Wine retries on `EINTR`, which is what it does for Linux's non-`SA_RESTART`
case (probe G1/G2).  A grant wins over a pending signal, as on Linux.

### 2.4 A thread that dies while parked

A SIGKILLed or exit_group'ed thread never re-executes, so its record stays
queued.  Every waker checks the record's thread (slot generation and state)
before granting: a dead thread is never handed a semaphore count or a mutex
(Linux unqueues at exit; probe F4), and a skipped dead record triggers a
sweep that unlinks every dead record and drops its references.  The slot's
next wait also replaces a stale record.

### 2.5 What it costs

Probe (guest, 8 vCPU): an event ping-pong between two threads 5.6-7.1 us per
round trip; 8 threads contending one mutex through `WAIT_ANY`/`UNLOCK`
3.0-3.7 us per acquire+release pair.  For scale: C1-k6's `linux_probe_ipclat`
wineserver-shaped request/reply round trip is 4-6 us on integ10, and a
server-based Win32 wait is at least one such round trip plus the server's
own work and, for a blocking wait, a second wake through the server's
`wait` pipe.

## 3. The probe (`src/user/linux_probe_ntsync.c`)

116 graded rows (N0-X5) plus one informational one (G5); the same source builds with
`musl-gcc` for the guest and with the strix host's `gcc` against
`/usr/include/linux/ntsync.h`, where `_Static_assert`s pin every ioctl
number and struct offset to the real UAPI (they compile:
`docs/cp-ntsync-evidence/probe-host-strix.txt`).  Ported by hand from the
upstream selftest (`tools/testing/selftests/drivers/ntsync/ntsync.c`;
no network here):

| group | selftest | rows |
|---|---|---|
| S1-S15 | `semaphore_state` | create validation, READ, RELEASE (prev count, `EOVERFLOW` incl. u32 wrap), waits take one, wrong-type ops `EINVAL`, `EFAULT` |
| M1-M15 | `mutex_state` | create validation, recursion, UNLOCK `EINVAL`/`EPERM`/prev counts, KILL, `MUTEX_READ` `EOWNERDEAD`, acquiring an abandoned mutex, count `UINT32_MAX` |
| E1-E14 | `manual_event_state`, `auto_event_state` | SET/RESET/PULSE prev states, auto-reset consumption, booleans |
| A1-A10 | `test_wait_any` | array order, duplicates allowed, count 0, abandoned mutex at index 1 |
| L1-L8 | `test_wait_all` | all-or-nothing (no partial consume), manual vs auto, duplicates `EINVAL`, abandoned |
| I1-I8 | `invalid_objects` | the device / a pipe / -1 as objects, pad/flags/count, `EFAULT`, `ENOTTY`, an object of another device, dup |
| W1-W17 | `wake_any` | sem/mutex/kill/event/pulse wakes of a parked waiter, MONOTONIC and REALTIME timeouts, close while waited on, fan-out (manual all, auto exactly one, sem 2 of 3), FIFO, wake latency |
| B1-B5 | `wake_all` | a parked WAIT_ALL needs all four, takes all at once; WAIT_ALL before WAIT_ANY; an unsatisfiable WAIT_ALL takes nothing |
| R1-R9 | `alert_any`, `alert_all` | alert index = count, objects win over the alert, count 0 + alert, parked waits ended by the alert, alert duplicating an object |
| G1-G5 | (HobbyOS) | a handled signal: `EINTR`; SA_RESTART: restart (Linux) or `EINTR` (here); pulse+signal races; signal-to-`EINTR` latency against a pipe read; (info) signal latency to a running thread |
| F1-F4 | (Wine's shape) | device + objects over SCM_RIGHTS to a forked "client" that waits and signals back; a forked child sharing a mutex; the server's MUTEX_KILL; a SIGKILLed waiter is never granted the release |
| X1-X5 | `stress_wait_any/all` | 8 threads x 20000 mutex acquire/release through WAIT_ANY and WAIT_ALL (exact counter); event ping-pong; CREATE_EVENT + close with 0 and 1500 objects open (the wineserver's price per internal sync) |

Guest (strix, `docs/cp-ntsync-evidence/probe-guest-strix.txt`): every row
PASS (HEAD and every earlier run).  Selected numbers: W8/W9 100 ms timeouts
end after 99.99 ms; W17 EVENT_SET -> waiter running again p50 12-16 us; G4
signal -> `EINTR` p50 44-46 us (a pipe read: 42-48 us); X1/X2 3.0-3.8 us per
acquire+release with 8 contending threads; X3 5.6-7.1 us per ping-pong round
trip; X4/X5 1.0-1.6 us per CREATE_EVENT + close.  On the base kernel
(integ11, measured) the probe fails at N0 with `ENOENT`, as on the strix
host, whose `ntsync.ko` is not loaded.

## 4. Wine with ntsync (`tools/cp/ntsync/build-ntsync-wine.sh`)

`strix:~/cp/lanes/ntsync-wine` (the script's output:
`docs/cp-ntsync-evidence/wine-build-strix.txt`):

* `build-x86_64/` and `stage/` copied from `~/cp/wine-build-x` (which is
  never written); the UAPI header vendored as
  `build-x86_64/include/linux/ntsync.h` (the build's own `-Iinclude`).
* What configure would decide with the header present: the original
  configure line, re-run in a scratch dir with the original environment
  (`tools/cp/build-wine.sh`'s: `CC`, `CFLAGS`, `LDFLAGS`, pkg-config on the
  sysroot and `x11-build-x2/pc`) plus `-I<header dir>`: `config.h` differs in
  exactly one line, `#define HAVE_LINUX_NTSYNC_H 1`.
* That line flipped in the copy with config.h's mtime kept; the only two
  objects that test the macro (`server/inproc_sync.o`,
  `dlls/ntdll/unix/sync.o`) rebuilt; `wineserver` and `ntdll.so` relinked.
* Staged the way `make install` does (`tools/install --strip-program=strip`
  = `strip -g`; proven byte-identical on the original pair first).  The
  stage differs from `~/cp/wine-build-x/stage` in those two files only.
* The build's scratch (1.3 GiB `build-x86_64/`) was deleted at the end of the
  lane (§8); the stage (204 MiB) stays for the integrator.

**In the guest** (`docs/cp-ntsync-evidence/p2-strix.txt`): Gate P2 on the
lane kernel with this stage passes (G1-G3 + the C:\ winprobe), the kernel
logs `[NTSYNC] open /dev/ntsync ... name=wineserver` for each wineserver,
and the stats rows count thousands of objects (P2: 7031 events, 33 mutexes
and 6 semaphores created, ~480 live at once) and waits.  **Fallback**: the same stage on
the base kernel (integ11, no `/dev/ntsync`): P2 passes, no `[NTSYNC]` row,
i.e. `get_inproc_device_fd()` failed and every object stayed a server
object.

## 5. Measurements

### 5.1 fpsprobe A/B (`docs/cp-ntsync-evidence/fps-ab-strix.txt`)

P9 defaults (`-smp 8 -m 32G` under the big-guest lock, X screen 1280x720,
zero-copy scanout taken in every boot), `VENUS_BUILD=~/cp/lanes/present-venus`
(the Venus Mesa the k7 / present lanes measure with), `tools/cp/p9/fpsprobe.p9`,
a prefix made by this tree's `P8_PHASE=pfx`, kernel = the lane at d6174e3f.
On = `CP_WINE_STAGE=~/cp/lanes/ntsync-wine/stage/CP/WINE`, off =
`~/cp/wine-build-x/stage/CP/WINE`; on/off/on/off/on/off back to back
(20:54-22:14), with 1-3 other lanes' guests on strix throughout.

| boot | world median (p10), rows | standing (prof) | wineserver run/kern | game run/kern (threads) | t96 / t92 run/kern % | load -> world |
|---|---|---|---|---|---|---|
| ns-s-on1 | 26.8 (23.5), 62 | 32.7 (29.9) | 0.07 / 0.06 | 2.62 / 1.04 | 94/15, 92/16 | 92.0 s |
| ns-s-off1 | 22.7 (17.9), 62 | 24.7 (19.5) | 0.29 / 0.26 | 2.26 / 0.91 | 86/16, 82/21 | 104.5 s |
| ns-s-on2 | 24.9 (22.8), 63 | 31.5 (28.2) | 0.08 / 0.07 | 2.57 / 1.01 | 93/15, 91/17 | 91.4 s |
| ns-s-off2 | 20.5 (17.5), 61 | 31.8 (30.4) | 0.30 / 0.27 | 2.26 / 0.90 | 87/15, 82/21 | 100.4 s |
| ns-s-on3 | 26.6 (20.9), 60 | 21.5 (16.3) | 0.07 / 0.07 | 2.57 / 0.94 | 94/13, 92/14 | 89.5 s |
| ns-s-off3 | 28.2 (14.9), 63 | 30.3 (27.1) | 0.30 / 0.27 | 2.30 / 0.96 | 86/17, 81/21 | 107.1 s |
| k7b-1 (k7 lane, integ11 + stock Wine, same harness) | 27.9 (19.6), 59 | 28.3 (24.1) | 0.30 / 0.27 | 2.27 / 0.90 | 86/15, 82/20 | 104.5 s |

`[THRW]` (world phase means, `tools/cp/ntsync/ab_summary.py` /
`hbv_summary.py`): with ntsync 12 game threads sit in `ioctl` (an ntsync
wait) where the stock Wine has 12 in `read` (the server's reply/wait pipe);
the group's "trap" share (in the kernel, not in a syscall) drops from
0.16-0.23 to 0.08-0.11 threads, READY from 0.23-0.27 to 0.21-0.25, and the
two hottest threads' READY from 4-7 % to 1-3 %.  The game's in-kernel `ioctl`
time grows from 0.37-0.39 to 0.53-0.59 threads: that is mostly the GPU fence
waiter (t121: 21-24 % against 15-19 % of wall time in `/dev/gpu` ioctls; it
waits for more fences per second at the higher frame rate).  The ntsync
calls themselves cost ~1 us each in the kernel (`ns/call=` on HEAD: 0.4-1.3
us averaged over a probe or thrstorm run), i.e. well under 1 % of one vCPU at the
prefix's ~5-6k calls a second.

Over a boot (~390 s from launch to the end of the walk) the prefix does
~400-450k ntsync waits (`any=`; no WAIT_ALL at all), ~1150 a second, half of
them satisfied without parking (`imm=`); ~1.4-1.5M EVENT_SET/RESET/PULSE
(~3800 a second, almost all the wineserver's internal syncs) and ~150k
semaphore releases; 0 lost wakes, 0 dead-thread skips; ~950 objects live at
most.

### 5.2 thrstorm, P2, P5 (same kernel, ntsync vs stock)

| | ntsync | stock |
|---|---|---|
| thrstorm rounds in 60 s (two pairs: a6efd84e, e779730a) | 821, 973 | 847, 781 |
| thrstorm: wineserver run (kern) during the storm | 0.71-0.72 (0.61-0.63) | 0.71-0.81 (0.63-0.71) |
| Gate P2 G1-G3 + C:\ winprobe | PASS (4 boots: a6efd84e, the batch, HEAD, a debug build) | PASS (2 boots) |
| `wineboot -i` in P2 / thrstorm, ms | 20100, 13071, 16524, 14064 / 14639, 9966, 11222 | 9494, 29506 / 9403, 15551 |
| Gate P5 (vkcube, d3d11, d3d12, fullscr) | PASS (3 boots incl. the batch's 2) | PASS |
| Gate P4 G1-G3 | PASS | (integ11: PASS) |

thrstorm is bounded by the wineserver either way: half its workers talk over
named pipes, whose I/O is server work, so taking the waits out of the server
does not move it (0.71 vCPU both).  `wineboot` is dominated by host load
(29.5 s for one stock boot); see §8.

### 5.3 What is left between 26 and 30

The game's main thread and its busiest worker (t92, t96) are on a vCPU 91-94 %
of the world walk; 13-17 % of the wall time is kernel (futex 7-10 %,
rt_sigprocmask 1-2 %, clock_gettime <1 %).  Nothing else in the guest is near saturation
(wineserver 7 %, game READY 0.25 threads).  So with ntsync the remaining
gap is those two threads' user-mode work plus their futex/syscall entry cost
(C1-k7's lane: trap/syscall entry/exit), not Wine's synchronisation.

## 6. Gates

Batch `strix:~/cp/lanes/ntsync-gates.sh` (a copy of the integrator's
`~/cp/integ-gates14.sh` for the tree `~/cp/lanes/ntsync-g`, lane at 15fc4b93
= the driver; every Wine gate with `CP_WINE_STAGE=~/cp/lanes/ntsync-wine/...`,
Venus gates with this lane's `~/cp/lanes/ntsync-venus` built by the tree's
`tools/mesa/venus/build.sh`), plus reruns on HEAD e779730a
(`~/cp/lanes/ntsync-head.sh`, tree `~/cp/lanes/ntsync-dev`): the probe, P2,
thrstorm, P5 (ntsync and stock), P4.  The commits after 15fc4b93 add only the
`[NTSYNC]` diagnostics rows, the per-call accounting and probe rows.
Evidence: `gates-strix.txt`, `head-runs-strix.txt`.

| gate | integ11 (`integ-gates14.txt`) | lane |
|---|---|---|
| 1 build | ok | ok |
| 2 linux probes (t3, LP_TIMEOUT=300) | 57/57 | **58/58** (+ ntsync; console probe intact) |
| 3 ext2_gate | PASS | PASS |
| 4 unit | 463 run, 2 known reds (virtio_gpu_test.c:214/:244) | 463 run, the same 2 |
| 5 host tests | 38/40 | 38/40 |
| 6 castscan | 371, 0 new | 371, 0 new |
| 7 ARM build | ok | ok |
| 8 exec_gate, rsize_guard, rsize_scan | PASS | PASS |
| 9 pfx_gate | PASS | PASS |
| ext2w_gate, reg_gate, ext2_test_host, nfs_proto, panic_sink | PASS | PASS |
| exitgroup x3 | 3/3 | 3/3 |
| Gate P2 | PASS | PASS (batch; + 2 HEAD boots) |
| Gate P4 | PASS | batch: harness could not build `rawinput.exe` (no `src/` next to the stage, §8); HEAD rerun **PASS**, every `[P4G]` row as integ11's |
| Gate P5 x2 (vkcube, d3d11, d3d12, fullscr) | #1 PASS, #2 FAIL (services.exe memory fault) | **2/2 PASS** (+ HEAD ntsync and stock PASS) |
| Gate P6 Venus | PASS | PASS |
| thrstorm | PASS, 822 rounds | PASS, 961 rounds (+ HEAD 973 ntsync / 781 stock) |
| k5probes | PASS | PASS |
| P7 fullscreen | PASS | PASS |
| P8 prefix phase | PASS | PASS (prefix made by the ntsync Wine) |
| P8 lavapipe x2 | main menu x2 | **main menu x2** (+366 s, +426 s) |
| P8 Venus | main menu (+200 s) | **main menu** (+181 s; also `ns-dbg1`, +165 s) |

## 7. Commands (strix)

```
# Wine with ntsync (copies ~/cp/wine-build-x; never writes it)
OUT=$HOME/cp/lanes/ntsync-wine bash tools/cp/ntsync/build-ntsync-wine.sh
# the probe
LP_TIMEOUT=400 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh ntsync
# Gate P2 / thrstorm with the ntsync Wine (stock: CP_WINE_STAGE=~/cp/wine-build-x/stage/CP/WINE)
CP_WINE_STAGE=$HOME/cp/lanes/ntsync-wine/stage/CP/WINE WINE_EXPORT=/srv/nfs/cp-ntsync-p2 bash tools/cp/run-wine-p2.sh
CP_WINE_STAGE=$HOME/cp/lanes/ntsync-wine/stage/CP/WINE K5_EXPORT=/srv/nfs/cp-ntsync-ts K5_OUT=$PWD/obj/cp-k5/ts-ntsync \
  TIMEOUT_S=2400 bash tools/cp/k5/run-thrstorm.sh
# a fresh prefix in this tree first (see §8: a stale one breaks the scanout)
P8_EXPORT=/srv/nfs/cp-ntsync-p8 P8_PHASE=pfx TIMEOUT_S=2400 bash tools/cp/p8/run-p8.sh
# one fpsprobe boot (P9 defaults: 1280x720 X screen, scanout), ntsync on
P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=ns-s-on1 VENUS_BUILD=$HOME/cp/lanes/present-venus \
  CP_WINE_STAGE=$HOME/cp/lanes/ntsync-wine/stage/CP/WINE P8_EXPORT=/srv/nfs/cp-ntsync-p9 TIMEOUT_S=2400 \
  bash tools/cp/p9/run-p9.sh
# per boot: world fps, wineserver / game [THRW] shares, the [NTSYNC] row
python3 -I tools/cp/ntsync/ab_summary.py obj/cp-p9/ns-s-on1 obj/cp-p9/ns-s-off1 ...
```

Every command above ran with `PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16
TOOLS_SKIP=1`.

## 8. Facts for other lanes, traps, what is not done

* **A stale P8 prefix breaks P9.**  This tree's `obj/cp-p8/pfx-seeded.img`
  was left by the wave-5 C6-p7b lane (Nov 17 11:10).  With it, on both Wines,
  the game's swapchain never matched the 1280x720 fullscreen window, the
  kernel refused every `GPU_IOC_SCANOUT_BLOB` (`[P7] scanout refused window
  W W`: the window matched, the size did not; `[HBP] path=putimage refused=N`)
  and the P9 harness, which reads a refused-scanout 1920x1080 glass in the
  1280x720 layout, never saw "PRESS ... TO CONTINUE" (boots `ns-on1`,
  `ns-on2`, `ns-off1`: 7 minutes on the title art, stopped).  A fresh
  `P8_PHASE=pfx` (2 minutes) fixed it: scanout taken in every later boot.
  Any lane that reuses an old tree: regenerate the prefix before P9.
* **The Wine build tree for P4.**  `run-p4.sh` takes the Wine build dir from
  the stage path (`stage/CP/WINE` -> three levels up) and builds
  `rawinput.exe` with its `src/wine-11.0` and `build-x86_64`.
  `~/cp/lanes/ntsync-wine/src` is a symlink to `~/cp/wine-build-x/src`;
  `build-x86_64` is a symlink to `~/cp/wine-build-x/build-x86_64` (the copy
  that built the two ntsync binaries was deleted at the end of the lane: the
  P4 helper only needs the headers, import libraries and `winegcc`, which
  are identical).  Gate P4 with the ntsync stage: PASS (§6).  Rebuilding the
  ntsync binaries needs `build-ntsync-wine.sh` again (it re-copies).
* **Wine 11 + ntsync signals threads for system APCs.**  The server can no
  longer hand a system APC to a thread inside a server `select`
  (`is_in_apc_wait` is false while it waits in the kernel), so it sends
  SIGUSR1 (`server/thread.c queue_apc`); the thread's ntsync wait ends with
  `EINTR`, the handler runs the APC through a server call, and Wine waits
  again.  A P2 sees ~450-700 such `EINTR`s; a whole game boot ~210, almost
  all during start-up.  Measured cost per signal in the probe:
  45 us to `EINTR` (G4), the same as a pipe read; 43 us to a thread running
  in user mode (G5).
* **fds.**  Every ntsync object is one global file slot (`MAX_GLOBAL_FILES`
  16384) and one fd in the wineserver (RLIMIT_NOFILE, raised by the server to
  the hard 4096) plus one in each client that waits on it.  The game peaked
  at ~950 live objects (`[NTSYNC] max=`), the P2 prefix at ~500, thrstorm at
  2800 (256 threads x events + the server's per-thread alert events and
  per-fd internal syncs).  A game with more than ~3000 live sync objects
  would hit the wineserver's 4096: `STATUS_TOO_MANY_OPENED_FILES` at object
  creation.  Not seen; the counter is in every stats row.
* **wineboot timing is host-noise dominated.**  `wineboot -i` (P2,
  thrstorm, P8 prefix phase), same lane kernel: ntsync 10.0-20.1 s (8 boots,
  median 14.4), stock 9.4-29.5 s (5 boots: 9.4, 9.5, 13.3, 15.6, 29.5;
  median 13.3); integ batches 7.6-11.2 s.  If ntsync costs wineboot a few seconds it is not
  visible above the noise of a shared host.  The ntsync driver's own kernel time
  in a whole P2 is ~85 ms (22934 calls, 3.7 us average: `calls=` /
  `ns/call=` in the stats row), CREATE_EVENT + close costs 1-1.6 us (X4/X5),
  and no queued wait was ever satisfiable-but-asleep (`LOSTWAKE` never
  printed, `satisfiable=0` in every 2 s dump of a debug P2).  Not chased
  further.
* **Not done.**  `/dev/ntsync` is not `stat()`-able by path (Wine only opens
  it); `poll()` on an ntsync fd answers whatever fs.c answers for an unknown
  type (Linux: `DEFAULT_POLLMASK`; Wine never polls them); the SA_RESTART
  deviation of §2.3; the probe was not run against a real Linux ntsync
  driver (strix's `ntsync.ko` is not loaded and loading it is a host change:
  `sudo modprobe ntsync` would let the probe's host build grade itself
  against Linux -- the integrator's call).

## 9. Evidence (`docs/cp-ntsync-evidence/`, all strix)

| file | what |
|---|---|
| `probe-guest-strix.txt` | `linux_probe_ntsync` on HEAD (every row, the `[NTSYNC]` console rows) and on the base kernel (N0 `ENOENT`) |
| `probe-host-strix.txt` | the probe built on the strix host against `<linux/ntsync.h>` (UAPI asserts compile; no `/dev/ntsync` there) |
| `wine-build-strix.txt` | `build-ntsync-wine.sh`'s run: header sha256, configure's one-line `config.h` diff, the two rebuilt objects, strip proof, stage diff |
| `p2-strix.txt` | Gate P2 with the ntsync Wine (lane kernel) and its fallback on the base kernel |
| `thrstorm-ab-strix.txt` | thrstorm, ntsync vs stock, two pairs, with `[THRW]` |
| `head-runs-strix.txt` | HEAD e779730a: probe, P2 / thrstorm / P5 with both Wines, P4 |
| `p8-venus-ntsync-strix.txt` | P8 on Venus with the ntsync Wine: menu timeline, `[NTSYNC]` rows, `[THRW]` at the menu |
| `fps-ab-strix.txt` | the six fpsprobe boots (and the three stale-prefix ones): fps per phase, `[THRW]`, timelines, `[NTSYNC]` rows |
| `gates-strix.txt` | the lane gate batch next to integ11's, and the batch script |
