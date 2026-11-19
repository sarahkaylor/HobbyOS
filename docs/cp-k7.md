# cp-k7 — the per-trap cost under the game (lane C1-k7, wave 6)

Plan: `docs/cyberpunk.md` §6 P9 (≥ 30 fps in the world), §7.2f.  Branch
`cp-k7` from integ11 @ 13dc693b.  Evidence: `docs/cp-k7-evidence/`.  Every
number is from **strix** (Ryzen AI MAX+ 395, Zen 5; host kernel
7.0.0-43-generic): guests are intel, KVM `-cpu host`, 8 vCPU; probe boots
`MODE=test`, 12 GiB, under `tools/cp/qemu-slot.sh`; game boots P9's
fpsprobe (32 GiB, Venus, `/tmp/cp-bigguest.lock`).  "Host" = the strix
Linux kernel running the same static musl binary.

## 1. Result

* **Per call:** the fixed trap cost on one thread is 158 ns (strix host
  Linux: 39 ns); 120 ns of the difference is fixed path, 70 ns of it the
  `iretq` (§2.1; sysret is blocked by the GDT layout).  What made the
  game's kernel time large was not the single-thread path but
  **concurrency**: ~15 per-CPU arrays written on every trap shared one
  cache line across all CPUs, every user copy took the global vm_lock
  three times per page, and futex wake / poll notification took proc_lock
  even when nobody was waiting.  With those removed, getpid on 8 threads
  at once went 1359 → 158 ns, futex wake-nobody on 4 threads 1288 → 207
  ns, clock_gettime on 4 threads 430 → 169 ns, rt_sigprocmask 449 → 183 ns
  (§2-§5).
* **[THRW]** over-reported kernel time three ways (§4); fixed.  With the
  fix the wineserver is still ~90 % kernel (0.22-0.26 of a vCPU: `write`,
  `epoll_wait`, `read`); its round trip is the wake, not the calls (§6.1).
* **The game: level.**  fpsprobe, 5 boots each, alternated: world median
  of medians **integ11 27.9 (p10 16.3), lane 25.5 (p10 18.0)** -- inside
  the ±5 fps spread, which follows the host GPU (another lane's Venus guest
  at 71 % → 19.2).  The hot game threads' kernel share fell (t96 15-19 % →
  11-13 %, futex 5-6 % → 1-3 %, rt_sigprocmask 2-3 % → 0), but on integ11
  the frame is no longer kernel-bound: the guest is ~60 % idle in the world,
  `proc_lock` spin is 2 %, and the two hot threads spend ~70 % of the wall
  clock in their own user code; the rest is the Venus fence/present path
  (§6).  **The 30 fps median is not reached reliably, and this lane does
  not change that** -- the remaining levers are not the trap path.

## 2. What one syscall costs (`linux_probe_sysc`, new)

ns per call, p50 of 15 batches of 4000 calls (`docs/cp-k7-evidence/sysc-strix.txt`;
integ11 = `sysc-base1`, lane = `sysc-fx1` (091a8f18 + fbf2ceed), both on a quiet
host; the gate batch's run on the final tree, 8e03d766, agrees within a few
ns except M4f 455 -- that row moves 200-450 run to run with the futex trace
ring's one shared atomic):

| row | call | integ11 | lane | strix host |
|---|---|---|---|---|
| S0 | getpid | 168 | 158 | 39 |
| S2 | clock_gettime (raw syscall) | 204 | 170 | 63 |
| S3 | clock_gettime via libc | 203 (no vDSO) | 168 | 18 (vDSO) |
| S4 | futex WAKE, nobody waiting | 437 | 178 | 51 |
| S5 | read, empty O_NONBLOCK pipe | 236 | 191 | 63 |
| S6 | pipe write 8 + read 8 (two calls) | 521 | 456 | 158 |
| S7 | rt_sigprocmask(SIG_BLOCK, {}) | 449 (309-513) | 183 | 46 |
| S8 | epoll_wait(1 idle fd, 0) | 177 | 176 | 46 |
| S9 | poll(NULL, 0, 0) | 167 | 164 | 72 |
| S10 | unknown number (ENOSYS) | 222 | 219 | 37 |
| M4 | getpid, 4 threads at once | 307 (p90 863) | 159 | 40 |
| M8 | getpid, 8 threads at once | **1359** | **158** | 40 |
| M4f | futex wake-nobody, 4 threads | **1288** | **207** | 51 |
| M4c | clock_gettime, 4 threads | 430 (p90 983) | 169 | 64 |

S11 (new in 5173062a): S6 while another thread sleeps in `poll()` on an
unrelated pipe -- integ11 634 ns (its S6 515), lane 492 (S6 487), the pair
under one load (`sysc-base2` / `sysc-pn1`).

### 2.1 Where one syscall's cycles go (`T0`/`T1`, test kernels)

`trap_prof` (arch/x64/trapprof.c, test syscall 0x4B37): rdtsc marks along
the path; each segment is the time from the previous mark (one mark costs
~22 TSC cycles, so a segment of 22 is "nothing").  TSC 2.995 GHz.
getpid, cycles per call (lane, quiet host, `sysc-fx1`):

| segment | what runs there | cycles |
|---|---|---|
| entry leg (user rdtsc → handler) | `syscall`, swapgs, the frame build in `common_trap_wrapper`, `save_user_sp_helper` | 64 |
| trapenter | `ru_trap_enter` (rdtsc + divide), sync-wake nesting, lock-audit IRQ flag | 46 |
| diag | badvec entry stamp, torn-frame check, `gic_set_current_vector`, `watchdog_tick` (11 stamps + a 296-byte frame copy) | 47 |
| route | `rw_watch_arm_cpu`, the wake-all drain, the vector chain | 44 |
| syspre | `sync_lower`: last_user_pc, kernel_stack_low, heartbeat, the 6-argument tuple, the syscall ring | 45 |
| disppre | `linux_compat_dispatch` prologue: stack-parity check, per-call resets, table lookup | 22 |
| handler | `l_core_getpid` | 55 |
| disppost / sigexit / unwind | FAT exit checks, `process_signal_trap_exit`, lock audit, sync-wake flush | 23 / 22 / 22 |
| ruexit | `ru_trap_exit` (rdtsc + divide) | 49 |
| exit leg (handler → user rdtsc) | restore, the frame reshuffle, the exit frame check, swapgs, **iretq** | 210 |

Subtracting the marks (~22 each), the C path is ~165 cycles (55 ns) and
the exit leg alone is 70 ns: the `iretq`.  Linux returns from a syscall
with `sysretq`; this kernel cannot, because SYSRET derives SS from CS
(`STAR[63:48]+8` and `+16`) and HobbyOS's GDT has user code (0x1B) BELOW
user data (0x23): sysret would hand user mode SS = 0x13 (the kernel data
descriptor), which Wine's own `iretq` paths (it reads `%ss` into its
contexts) and every later `iretq` to user would #GP on.  Reordering the GDT
changes the user CS every gate and every `cs == 0x1B` check depends on:
not done here (§7).

Single-threaded, then, the trap path is not where the game's time goes:
158 ns against Linux's 39 ns, all of it a fixed ~120 ns.  What made the
kernel time large was **concurrency**: the M rows.

## 3. The fixed cost that grew with concurrency (fbf2ceed, 26dd5438)

### 3.1 Per-CPU arrays on one cache line

`nm` of the integ11 kernel: the per-trap stamps were `[MAX_CPUS]` arrays
whose eight entries share one 64-byte line (`ls_in_irq` was eight bytes for
all eight CPUs).  Every syscall and interrupt wrote ~15 of them:

| written on every trap | file | now |
|---|---|---|
| `wd_last_seen_ms/rip/rflags/vector/err/cr2/pid/tfp`, `wd_ticks`, `wd_last_frame` | trap.c (`watchdog_tick`) | `struct wd_cpu wd_pc[]`, 64-byte aligned |
| `badvec_at_entry`, `badvec_eu` | trap.c | struct aligned(64) |
| `core_in_syscall` | trap.c | padded; also cleared on the table-dispatch return (it was left set) |
| `cpu_heartbeat_ms` (59 users in 10 files) | process.c | stored only when the millisecond moves |
| `current_vector` | gic.c | stored only on a change |
| `user_sp` | cpu.c | padded |
| `sched_trap_depth/sys`, `sched_defer_slot/until`, `sched_kick_pending` | process.c / trap.c | `struct sched_trap_pc sched_tpc[]` (process.h) |
| `ls_in_irq`, the held stack, the IRQ-off window, `ls_preempt` | lockscale.c (every trap; every spinlock pair) | `struct ls_cpu_state ls_pc[]` |
| `held_lock_depth`, `held_locks/pc/pc2`, `plh` | locks.c (every spinlock pair; proc_lock) | aligned per CPU (the monitor probe's layout unchanged) |
| `l_skw[].par` | linux_compat.c (stack-parity table) | stored only on a change |

Each store to such a line has to pull it from the CPU that wrote it last:
getpid on 8 threads at once was 1359 ns per call (p50; single thread 168);
it is now 158 (M8), level with one thread.

### 3.2 vm_lock on every copy to or from user memory

`l_copy_from_user`/`l_copy_to_user` validated with `vm_touch` (vm_lock,
region lookup, page walk) and then copied with `vm_user_read`/`vm_kwrite`
(per page: `vm_touch` again, vm_lock again, lookup, walk): three
acquisitions of one global lock per page for an 8-byte sigset or a
timespec, and `sys_user_range` (native read/write/clock_gettime) did a
fourth.  Every thread of every process serialised on it -- including the
Venus ioctl threads, which copy their argument structs the same way.

`vm_copy_fast` / `vm_range_fast_ok` (vm.c) + `vm_arch_fast_leaf` (mmu.c):
for the address space THIS CPU runs (its root in CR3), a present user leaf
that grants the access (and is not COW, and is in the direct map) is used
with no lock.  Interrupts are off from the lookup to the end of that page's
copy; munmap/mprotect/mremap drop or downgrade a leaf and only then free
the frame after a shootdown that waits for every CPU running the AS
(C1-k4), so the frame cannot go away mid-copy -- the guarantee a user-mode
access on this CPU already has.  Page tables of a live AS are freed only at
teardown.  Anything else (absent page, read-only for a write, COW, a GPU
blob outside the direct map, another process's AS) takes the old locked
path unchanged, which materializes, breaks COW or refuses as before.

rt_sigprocmask 449 → 183 ns; clock_gettime on 4 threads 430 (p90 983) →
169 ns.  Residual risk, same as for user-mode stores on another CPU today:
the object-truncation path (`vm.c` ~635, `vm_arch_unmap` of private COW
copies past a shrunk file's end) frees a frame before any shootdown.

## 4. [THRW] and getrusage attribution (fbf2ceed)

Three biases in C6-present's sampler, all toward "kernel":

1. A thread resumed into user mode by `enter_user_space` (after a park in
   a syscall, or a preemption) kept `ru_kin_us` set until its next trap, so
   the sampler counted its user code as kernel time.  The wineserver parks
   in `epoll_wait` for every request and then runs user code: this is how
   it read as ~95 % kernel.  `ru_resume_user()` (from `resume_preflight_c`)
   closes the stretch.  (getrusage itself was barely affected: the next
   `ru_trap_enter` overwrote the stale stamp rather than charging it.)
2. The thread the sampling tick (CPU 0) interrupted in user mode had just
   been stamped by that tick's own entry: every thread on CPU 0 counted as
   kernel.  The tick now tells the sampler (`cpw_tick_user`).
3. "In a syscall" vs "a fault or interrupt" was read from the CPU's last
   syscall number: `read` (nr 0) looked like a trap, and a table-dispatched
   syscall left its number behind after returning.  Threads carry `in_sys`
   now.

So integ11's `[THRW]` kernel shares are upper bounds; the lane's are the
corrected numbers (§6).

## 5. Fewer global locks per wake (091a8f18, 5173062a)

* **FUTEX_WAKE with nobody waiting** took proc_lock and scanned all 512
  slots.  `l_fx_cnt[256]` counts queue keys per hash bucket (every write of
  a slot's `uaddr` goes through `l_fx_key_set`); WAIT counts itself before
  comparing the word (a locked RMW), WAKE fences after the caller's store
  and reads the count: either WAKE sees the waiter (locked path) or WAIT
  sees the new value (EAGAIN).  Over-counting (a stale key after a timeout,
  a collision) only takes the locked path.  437 → 178 ns; 4 threads 1288 →
  207 ns.  The trace printer reads before its atomic.
* **poll notifications**: every pipe write notifies parked pollers, and
  with any poller parked anywhere it took proc_lock to scan them; it now
  looks lock-free for one whose key filter matches first (the other half
  of `poll_park_bloom_locked`'s Dekker pair).  S11 (S6 with an unrelated
  poller parked): integ11 634 ns (S6 515), lane 492 (S6 487).

Not changed: the wake itself (`process_wakeup`, `proc_mark_ready`, the IPI)
still takes proc_lock once per woken thread, and `schedule()` still runs
under it.  `linux_probe_ipclat` is level (I1 5 µs, I3 7 µs, I5 63-65 µs on
both kernels under the same load, `ipclat-strix.txt`): the wineserver round
trip is the wake and the switch, not the per-call costs.

## 6. The game: fpsprobe A/B (strix, Venus, 32 GiB, fpsprobe.p9)

Base = integ11 13dc693b (`~/cp/lanes/k7-base`), lane = this branch at
5173062a (`~/cp/lanes/k6`), boots alternated, the same guest Mesa
(`~/cp/lanes/present-venus`) for both.  World = the scripted 90 s walk;
median (p10) of winevulkan `trace:fps`.  The host sampler (from k7b-2 on)
reads the 9070 XT's `gpu_busy_percent` each second during the walk and
lists the other lanes' guests (`fpsprobe-ab-strix.txt`,
`host-during-world-strix.txt`).

| boot (in order) | kernel | world median (p10) | 9070 XT busy (median/p90) | other guests |
|---|---|---|---|---|
| k7b-1 | integ11 | 27.9 (19.6) | — | — |
| k7l-1 | lane | — game exited 2.5 s after launch (REDEngineErrorReporter, §7) | — | — |
| k7l-2 | lane | 25.5 (22.0) | — (Venus fence waits 43 ms/frame: GPU shared) | — |
| k7b-2 | integ11 | 28.8 (15.3) | 27 / 33 | integ (12 GiB, Venus) |
| k7l-3 | lane | 19.2 (16.5) | **71 / 78** | armchk (12 GiB, Venus) |
| k7b-3 | integ11 | 34.0 (16.3) | 33 / 39 | p7c |
| k7l-4 | lane | 29.5 (20.8) | 31 / 36 | ntsync-g, regress |
| k7b-4 | integ11 | 24.6 (10.9) | 30 / 34 | regress-ref |
| k7l-5 | lane | 25.7 (18.0) | 32 / 36 | regress |
| k7l-6 | lane | 24.0 (14.7) | 27 / 33 | ntsync-g |
| k7b-5 | integ11 | 25.6 (18.5) | 33 / 79 | audio-g, ntsync-g |

Median of medians (5 boots each): **integ11 27.9 (p10 16.3), lane 25.5
(p10 18.0)**; without k7l-3, which ran while another lane's Venus guest
held the 9070 XT at 71 %, the lane is 25.6 (p10 17.4) over 4.  Level within
the ±5 fps boot-to-boot spread (Mann-Whitney on the medians, 4 vs 5: U = 7,
far from significance); the p10s lean the other way.  **30 fps is not reached reliably on either
kernel**, and this lane does not move it.

Why, from the same boots (`thrw-hbv-world-strix.txt`, `prof-world-strix.txt`):

| world, per boot | integ11 (5 boots) | lane (k7l-2..6) |
|---|---|---|
| guest vCPUs idle / kernel / user (standing window, `prof.py`) | 59-64 / 18-23 / 17-19 % | 54-65 / 17-28 / 18-19 % |
| `spin_acquire_raw` | 1.4-2.7 % | 1.7-3.5 % |
| top kernel symbol | `gpu_m2_fence_wait2` 3.9-7.4 % (the Venus fence kick-spin) | the same, 4.0-6.3 % |
| game thread t96: running / in the kernel | 85-88 % / 15-19 % (futex 5-6, sigprocmask 2-3) | 83-88 % / 11-13 % (futex 1-3, sigprocmask 0) |
| game thread t92 | 81-84 % / 20-22 % (futex 5-6) | 78-83 % / 16-18 % (futex 2-4, read 3-4) |
| Venus ioctl threads (t121, t165) in the kernel | 0.6-1.3 threads | 0.4-1.1 threads |
| wineserver running / kernel / of which "trap" | 0.25-0.31 / 0.23-0.28 / 0.08-0.10 | 0.25-0.29 / 0.22-0.26 / 0.03-0.04 |

(integ11's `[THRW]` carries §4's biases toward kernel time; the lane's
does not, so the per-thread kernel drop is partly measurement.  The
per-syscall split is not biased that way: futex 5-6 % → 1-4 %,
rt_sigprocmask 2-3 % → 0.)

So on integ11 the frame is **no longer bound by the guest kernel**: the
guest is ~60 % idle in the world, `proc_lock` spin is 2 % (it was 7-12 %
on integ10), and the two hot game threads are 83-88 % on a vCPU with
only 11-18 % of the wall clock in the kernel -- i.e. ~70 % in their own
user code, which this lane cannot shorten.  The rest of the frame is the
GPU path: per frame the vkd3d fence waiter waits 7-8 ms (40 waits), the
frame fence 4 ms, present `sub` 4-6 ms, flip 3 ms; and in a boot where
another guest loads the 9070 XT those triple (k7l-2: fence waits 42.9
ms/frame, sub 13 ms).  The boot-to-boot spread follows the host GPU.

### 6.1 The wineserver (task item 3)

With §4's attribution fixed, the wineserver is still ~90 % kernel: 0.22-0.26
of a vCPU in the kernel out of 0.25-0.29 running.  The split (lane, world):
`write` 8-11 %, `epoll_wait` 6-7 %, `read` 0-2 %, faults/interrupts 3-4 %
of the wall clock.  That is the reply write (pipe copy, waking the client
blocked in `read` -- `process_wakeup` takes proc_lock -- and notifying
pollers) and the epoll wait/park/resume.  `recvmsg`/`sendmsg` (SCM_RIGHTS)
and futex do not appear.  This lane removed what was per-call overhead in
them (the vm_lock copies, the poll-notification lock, the false sharing);
`linux_probe_ipclat`'s round trip is unchanged (§5), because what is left
is the wake: proc_lock per woken thread, the sync-wake window, and the
LAPIC writes (`hrt_arch_arm_us` 0.6-0.9 %, `sched_wake_idle_cpu` 1.3-1.6 %
of all samples -- xAPIC MMIO, i.e. VM exits).  The structural fix is
taking the wineserver out of Win32 waits (ntsync / esync, another lane's
item) rather than a faster round trip.

## 7. Not done, and why

* **sysret.**  70 ns of the 158 ns is the `iretq` exit.  SYSRET loads
  CS = STAR[63:48]+16 and SS = STAR[63:48]+8, so user data must sit just
  below user code in the GDT; HobbyOS has user code 0x1B below user data
  0x23.  Making sysret work means a GDT reorder that changes the user CS
  (every `cs == 0x1B` check, the torn-frame check, enter_user_space's
  stamps, Wine's `%ss`-reading iretq paths): a lane of its own.
* **A per-CPU run queue (item 4).**  Not started: proc_lock spin in the
  world is 1.4-3.5 % of samples on both kernels, i.e. it is not where the
  frame goes now.
* **vDSO.**  The guest has none (S3: clock_gettime through libc is a
  syscall, 168 ns; Linux's vDSO 18 ns).  In the world `clock_gettime` is
  0-3 % of the hot threads' time on the lane; not on the critical path.
* **The wake path.**  `process_wakeup` / `proc_mark_ready` still take
  proc_lock per woken thread, and the sync-wake window arms the LAPIC
  one-shot (xAPIC MMIO: a VM exit) per deferred wake; `hrt_arch_arm_us`
  + `sched_wake_idle_cpu` are 2-2.5 % of all world samples.  x2APIC (MSR
  writes, KVM's fast paths) would make those exits cheaper; untried.
* **No probe row for §4.**  A thread resumed after a futex park and
  spinning in user mode reads correctly through getrusage on integ11 too
  (the next trap overwrote the stale stamp instead of charging it), so a
  rusage row cannot see the bug; only the sampler could.  Tried
  (`linux_probe_rusage` R6, both kernels PASS) and dropped.
* **In-kernel halts in two lane boots.**  `safe_wfi` (a halt on behalf of
  a claimed slot -- a syscall or kernel task waiting on a device, not the
  idle loop) is 6.1-7.3 % of the world samples in k7l-3 and k7l-6 and
  0.8-1.7 % in k7l-4/5; 1.0-1.8 % in all five integ11 boots.  k7l-3 ran
  with the 9070 XT 71 % busy (longer GPU waits); k7l-6 did not (27 %).
  fpsprobe's PC sampler records no stack words, so whose halts they are is
  not known; this lane changes no wait site.  Worth one
  `fpsprobe-stack.p9` boot if it recurs.
* **The early exit (k7l-1).**  The game spawned `REDEngineErrorReporter`
  2.5 s after launch (`[P8] end game status=0 ms=2467`), on the lane
  kernel, 1 of 6 lane boots (0 of 5 integ11 boots here).  The integrator's
  note has the same signature once in 3 P8 lavapipe runs on integ11, so it
  is not obviously this lane's; no kernel fault row, 20 `[BADELR]` rows
  (the same count as every boot, base included).  Timeline:
  `k7l-1-timeline.txt`.

## 8. Commands (strix)

```
# the probe (guest; the strix host runs the same ELF: obj/intel/probe_sysc.elf)
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh sysc
# one fpsprobe boot (~/cp/lanes/k7-p9.sh: + a host sampler during the walk:
# other guests, 9070 XT gpu_busy_percent each second, loadavg)
P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=k7l-4 VENUS_BUILD=$HOME/cp/lanes/present-venus \
  P8_EXPORT=/srv/nfs/cp-k7-p9 PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 \
  bash tools/cp/p9/run-p9.sh
# A/B: base = ~/cp/lanes/k7-base (git archive of 13dc693b, third_party -> k6's),
# lane = ~/cp/lanes/k6; boots alternate (~/cp/lanes/k7-ab.sh k7b-3 k7l-4 ...)
python3 -I tools/cp/present/hbv_summary.py obj/cp-p9/<run>      # [HBV] + [THRW] per phase
python3 -I tools/cp/p9/prof.py obj/cp-p9/<run>/prof-fp-world.txt hobbyos.elf
```

## 9. Gates (strix; parity reference integ11 = `~/cp/integ-gates14.txt`)

Batch `~/cp/lanes/k7-gates.sh` = `integ-gates14.sh` for `~/cp/lanes/k6`,
`cp-k7-*` exports, the Venus runs on `~/cp/lanes/present-venus` (this lane
changes no Mesa), P8 lavapipe ×2 and the `p8_pfx` phase left out (the
brief asks for P8 on Venus).  Tree 8e03d766.  Raw: `gates-strix.txt`.

| gate | lane 8e03d766 | integ11 (gates14) |
|---|---|---|
| 1 build (MODE=test) | ok | ok |
| 2 every `linux_probe_*` (58 incl. the new sysc) | 57 PASS; **console** torn 6 / missing 45 under load → **rerun PASS** (0/0, 27162/27162) | 57 PASS (console 0/0) |
| 3 ext2_gate | **FAIL (2)**: `/lpx/glibc/ld-linux-x86-64.so.2` staged empty -- the shared `~/cp/sysroot-glibc231/lib64/ld-linux-x86-64.so.2` now points into `~/cp/lanes/ext2b/obj/`, which is gone (strix disk cleanup); **rerun PASS** with a private copy of the sysroot whose lib64 link points at its own `lib/x86_64-linux-gnu/ld-2.31.so` | PASS (before the cleanup) |
| 4 unit | 463 run, 2 failed: `virtio_gpu_test.c:214/:244` | same |
| 5 host | 38/40, kconsole PASS | same |
| 6 castscan | 373, 0 new (11 rows renamed/added, §3) | 371, 0 new |
| 7 ARM build | ok | ok |
| 8 exec_gate / rsize_scan / rsize_guard | exec_gate **FAIL** (the same glibc dyn ELF row, same sysroot cause) → **rerun PASS** (private sysroot); PASS; PASS | PASS ×3 |
| 9 pfx_gate | PASS | PASS |
| ext2w_gate / reg_gate / ext2_test_host / exitgroup ×3 / nfs_proto / panic_sink | PASS / PASS / PASS / 3 PASS / PASS / PASS | PASS / … |
| P2 | G1 G2 G3 PASS | PASS |
| P4 | G1 G2 G3 PASS | PASS |
| P5 ×2 (vkcube, d3d11, d3d12, fullscr) | PASS, PASS | PASS, FAIL (a services.exe fault row) |
| P6 Venus (vkcube, d3d11, d3d12) | PASS | PASS |
| P7 fullscreen on Venus | PASS | PASS |
| thrstorm (256 Wine threads, 60 s) | PASS, **855 rounds** | PASS, 822 rounds |
| k5probes (P8ENV, P8SCM, E5) | PASS | PASS |
| **P8 on Venus** | **main menu** (OCR ×2, launch + 124 s) | main menu |
| P9 fpsprobe (§6) | 5 of 6 boots to the world; 1 early exit (§7) | 5 of 5 |

## 10. Evidence (`docs/cp-k7-evidence/`)

| file | what |
|---|---|
| `sysc-strix.txt` | `linux_probe_sysc` at each step of the lane, integ11, the lane/base pair under one load, and the strix host |
| `ipclat-strix.txt` | `linux_probe_ipclat`, lane vs integ11 under the same load, and integ14's |
| `fpsprobe-ab-strix.txt` | the 11 fpsprobe boots, kernel-labelled, with host GPU load |
| `host-during-world-strix.txt` | per boot: other guests, 9070 XT `gpu_busy_percent` each second of the walk, loadavg |
| `thrw-hbv-world-strix.txt` | per boot, world: `[HBV]`/`[HBP]` per frame and `[THRW]` per group/thread |
| `prof-world-strix.txt` | per boot: guest PC profile of the standing window |
| `k7l-1-timeline.txt` | the early exit (REDEngineErrorReporter 2.5 s after launch) |
| `gates-strix.txt` | the gate batch |
