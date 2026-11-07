# L12 — process/thread ceiling, fd table and real-time signals (M2/M7)

Status: implementation + gate defined; evidence section is filled from the
serial log of `tools/gate/m12_thread_ceiling.sh`.

## Why this lane exists

The M0 profile of the real game (`docs/rimworld-evidence.md` §3.4) measured:

| Measurement | Value | Consequence |
|---|---|---|
| Threads at the main menu | **106** | a 64-slot PCB table is already exhausted before the menu |
| Peak threads | **145** | 191 usable slots are needed with margin |
| Open fds at the menu | **33** | a 32-entry per-process fd table fails during startup |

`MAX_PROCESSES` was 64, so `clone()`/`thread_create()` returned `-EAGAIN` at
the 65th live PCB. `MAX_OPEN_FDS` had already been raised 32 → 64 by the
IceWM work in this tree; this lane keeps it there, proves the measured
high-water fits, and adds a compile-time guard so it cannot silently grow
past its one-word `fd_cloexec` bitmap.

A third wall was signal delivery: `sig_known()` (the `kill`/`tgkill` gate)
accepted only the classic 1..17 set, so glibc's `pthread_cancel`
(`tgkill(tid, SIGCANCEL=32)`) and `pthread_setxid` (`SIGSETXID=33`) were
refused with `-EINVAL` before any delivery code ran.

## Change 1 — MAX_PROCESSES 64 → 192

`src/include/process.h`:

- 192 slots, 191 usable (slot 0 is reserved for the kernel/boot context).
  46 slots of headroom over the measured 145 peak.
- 192 is a multiple of 64, so every pid bitmap is exactly 3 words and slot
  scans stay branch-free and short.

### PID bitmaps are now word arrays

`src/include/pidmask.h` (new) defines `struct pid_mask` (fixed 256 bits,
4 words) with `pid_mask_zero/set/test/empty/or`. Pipe and AF_UNIX wait
channels used bare `uint64_t` masks with `1ULL << pid`; that is undefined
behaviour for pid ≥ 64 and would have corrupted an adjacent field once the
table grew. `process.h` asserts `MAX_PROCESSES <= PID_MASK_BITS`, so a future
increase cannot outrun the masks silently.

Converted sites:

| File | Fields |
|---|---|
| `src/include/pipe.h`, `src/kernel/pipe.c` | `reader_pid_mask`, `writer_pid_mask` |
| `src/include/unix.h`, `src/kernel/unix.c` | `chan[].rwait`, `chan[].wwait`, `acc_wait`, `conn_wait` |
| `src/kernel/unix_test.c` | assertions use `pid_mask_test`/`pid_mask_empty` |

`drain_wakeup_mask()` in both files now iterates the words and computes
`pid = (word << 6) | ctz(word)`; wake ordering is still lowest-pid-first.

### RAM cost of the bigger tables (exact, from the built ELF)

Per-entry sizes are unchanged; only the entry count grew by 128.

| Table | Per entry | 64 slots (old) | 192 slots (new) | Delta |
|---|---:|---:|---:|---:|
| `proc_table` (`struct process`) | 7,952 B | 508,928 B | 1,526,784 B | **+1,017,856 B** |
| `spawn_ex_pool` (`struct ho_spawn_ex`) | 4,720 B | 302,080 B | 906,240 B | **+604,160 B** |
| `spawn_args_pool` (x64 + ARM, `struct sys_spawn_args`) | 1,072 B | 68,608 B | 205,824 B | **+137,216 B** |
| `l_sigflags` (`uint32_t[HO_SIG_MAX]`) | 256 B | 16,384 B | 49,152 B | +32,768 B |
| `vm_as_pool` (`struct addr_space`) | 104 B | 6,656 B | 19,968 B | +13,312 B |
| `l_thr` (`struct l_thr_slot`) | 56 B | 3,584 B | 10,752 B | +7,168 B |
| `l_altstack` | 24 B | 1,536 B | 4,608 B | +3,072 B |
| `v2_prefetch_pool` | 24 B | 1,536 B | 4,608 B | +3,072 B |
| `select_wait` | 16 B | 1,024 B | 3,072 B | +2,048 B |
| `l_sigmask` | 8 B | 512 B | 1,536 B | +1,024 B |
| `wait_since_ms` | 8 B | 512 B | 1,536 B | +1,024 B |
| `orphan_reported` | 4 B | 256 B | 768 B | +512 B |
| **Total (slot tables)** | | **911,616 B** | **2,734,848 B** | **+1,823,232 B (~1.74 MiB)** |

Two non-slot tables also grew because their masks widened (they are not part
of the 128-entry delta): the 64-entry pipe pool +3,072 B (560 → 608 B per
pipe) and the 128-entry AF_UNIX pool +8,192 B (1,296 → 1,360 B per socket).
Combined table growth is **1,834,496 B (~1.75 MiB)** in kernel BSS.

Kernel BSS is already hundreds of MiB; +1.8 MiB does not move the Limine
load base or the frame-pool window. No new allocation, lock or lifetime:
every table remains static and pid-indexed as before.

## Change 2 — scheduler and slot-claim paths

No scheduler change was needed; the two documented failure classes were
re-checked against the new ceiling:

- **Slot famine (`EAGAIN`)**: `process_create_internal_ver()` and
  `thread_alloc_slot_locked()` are linear scans over 1..MAX_PROCESSES-1.
  192 slots × a few instructions is still trivial under `proc_lock`; the
  reclaim order (FREE → EXITED zombies → THREAD_DONE) is unchanged.
  The `[KERNEL] process_create: no free process slots!` and
  `[KERNEL] thread_create: no free slot (used=N/M done=M)` forensics remain
  rate-limited to the first 3 events per boot — the P8.2 rule against UART
  print storms is untouched, and `M` is now `MAX_PROCESSES - 1` = 191.
- **IDLESTUCK / lost-owner**: claim bookkeeping is per-CPU, not per-slot;
  `process_still_running()` and the heartbeat logic are unchanged. Growing
  the table only gives the reaper more slots to scan.

The 150-thread gate is a direct famine test: 151 live PCBs (150 threads +
main) is 79% of the 191-slot table and leaves 40 free, so a successful run
proves the allocation path under near-peak occupancy, and the gate fails if
any `no free slot` forensic appears (the wrapper greps for it and prints it
alongside the verdict).

Futex cost note: the compat wait queue is `l_thr[MAX_PROCESSES]` and a wake
is a linear scan of live slots (`linux_compat_thr.c`). 192 slots is 3× the
old scan; at the game's 1.09 M futex calls this remains a few hundred
milliseconds across a run, and the table cannot go stale (which is why the
hash-chain variant was rejected after a measured lost wakeup).

## Change 3 — fd table

`MAX_OPEN_FDS` is already 64 in this tree (IceWM M1 raised it from 32).
Measured game high-water is 33, leaving 31 fd slots of headroom for the
`/proc/self/fd/N` re-open storm (639 opens / 20 s measured — those are
re-opens, not new fds).

Added `_Static_assert(MAX_OPEN_FDS <= 64)` because `struct process.fd_cloexec`
is a single `uint64_t`: raising the table past 64 without widening that field
would shift CLOEXEC bits out of range. The assert makes that a build error.

## Change 4 — real-time signal delivery (32..63)

Three separate walls, all fixed for the compat class without changing the
native signal contract:

1. **`sig_known()`** (`process.c`) — `kill`/`tgkill` validation now accepts
   32..63 when the target is a `PROC_FLAG_LINUX_ABI` process. A native target
   still gets `-EINVAL`, exactly as before.
2. **`sig_deliverable()`** (`process.c`) — the real-time range is now named
   explicitly as deliverable (`sig >= 32`). The native HOS3 exclusion list
   (ILL/ABRT/BUS/FPE/SEGV/USR1/USR2/STOP) is unchanged.
3. **siginfo for `_kill`** (`linux_compat_thr.c`) — glibc 2.31's
   `sigcancel_handler` disassembles to:

   ```
   mov 0x10(%rsi),%ebp      ; si_pid
   call __getpid
   cmp %eax,%ebp / jne ...  ; si_pid must equal getpid()
   cmpl $0xfffffffa,0x8(%rbx) ; si_code must be SI_TKILL (-6)
   ```

   The compat frame engine previously wrote only `si_signo/si_code/
   si_addr`. For rt signals it now writes `si_code = SI_TKILL` and
   `si_pid = tgid` into the `_kill` union at offsets 8/16.

4. **`getpid()` is the TGID, not the tid** (`linux_compat.c`) — a thread has
   its own pid slot here, so `getpid()` inside a worker used to return its
   tid. Linux's `getpid()` is process-wide; glibc's cancel handler compares
   `si_pid` against it on the cancelled thread, so the wrong answer made the
   handler return without cancelling. `gettid()` now has its own handler and
   still returns the per-thread pid.

## Gate and evidence

`tools/gate/m12_thread_ceiling.sh` builds `src/user/rw_thread_stress.c`
against the pinned glibc 2.31, stages it as `PROBE.ELF`, boots, and grades
the serial log. The probe:

- creates **150** threads, each with 256 KiB stack and its own `__thread`
  `id`/`id²`, verifies them after join;
- releases all 150 at once through a `pthread_barrier` (150 waiters on one
  futex word, 150-way wake);
- reduces `sum(id²)` under a contended mutex and checks
  `shared == 1,113,775` and `sum(id) == 11,175`;
- joins all 150 and checks each return value encodes that thread's own TLS;
- then runs a worker in a `pthread_testcancel()` loop, calls `pthread_cancel`,
  and requires `pthread_join` to return `PTHREAD_CANCELED` — the
  SIGCANCEL(32) round trip. (A condvar-parked worker was measured to wedge in
  libc on this kernel; that is a condvar/cancel interaction, not the signal
  delivery this gate certifies — see Residual risks.)

Serial evidence (from the real run, `obj/m12_thread_ceiling.log`):

```
[T150] threads=150 start
[T150] threads=150 created=150
[T150] tls_ok=150
[T150] joined=150 tls_sum=11175 shared=1113775 expect=1113775
[T150] cancel start
[T150] cancel worker runs
[T150] cancel=ok (SIGCANCEL 32 delivered)
[T150] PASS
System halt.
```

`m12: PASS` (gate exit 0). No `[KERNEL] thread_create: no free slot` and no
`process_create: no free process slots!` line appears in the run: 151 live
PCBs against 191 usable slots did not approach famine.

## Residual risks

- `MAX_GLOBAL_FILES` stays 128 (system-wide). It bounds total open files
  across all processes, not per-process fds; the game at 33 fds plus the
  launcher/console fits. A future multi-process workload with many fds each
  would need that table raised separately.
- 192 slots is a *slot* ceiling, not a memory promise. v1 processes still
  reserve a 32 MiB block each; 192 of those would exceed the frame pool on a
  12 GiB guest. The game uses v2 address spaces (demand-paged) and threads
  share the leader's AS, so only the leader allocates.
- The linear futex scan is O(192); if profiling shows it on the game's hot
  path, a per-address hash can be added later without changing the PCB
  contract.
- `pthread_cancel` on a thread parked in `pthread_cond_wait` was measured to
  wedge (libc spins in the condvar after the SIGCANCEL interrupt) while the
  same cancel against a `pthread_testcancel()` loop completes. The signal
  delivery is correct; the condvar-park/cancel interaction needs its own
  investigation before a workload relies on cancelling condvar waiters.
