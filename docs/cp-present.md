# cp/present — what the in-world frame waits for (lane C6-present, wave 6)

Plan: `docs/cyberpunk.md` §6 P9 (≥ 30 fps in the world at 1280×720 Low),
§7.2f.  Starting point: `docs/cp-p9.md` §10 (integ9 + the play lane's round
2: zero-copy scanout, world median 19–26 fps, vCPUs 44–55 % idle).
Every number is from **strix** (Ryzen AI MAX+ 395 / Zen 5, RX 9070 XT on
renderD129, QEMU 10.2.1, virglrenderer 1.2.0), guest `-smp 8 -m 32G -cpu
host` under KVM, Venus, X screen 1280×720, the user's 1280×720 borderless
window on the zero-copy scanout, with 1–3 other lanes' guests on the host.
Evidence: `docs/cp-present-evidence/`.

## 1. Result

**The frame does not wait on the present path, the Venus transport or the
host.**  On the cp-play kernel (integ9) it waited on two game threads that
were each ~85 % on a vCPU — a quarter of the wall time of each in the
HobbyOS kernel — and on runnable threads that sat READY for a CPU while
vCPUs were halted.  The READY half is the scheduler's, and integ10's C1-k6
round 2 removed most of it (READY 1.0–1.8 → 0.24 threads); C1-k6's
`9d47b051` (halts bounded to 1 ms) adds to it.

**fps (fpsprobe world walk, median (p10)), labelled by kernel** — one boot
each unless noted; the spread between boots of one configuration is ±5 fps
(1–3 other lanes' guests on strix, one of them at times holding the 9070 XT
at 90 % busy):

| kernel | this lane's changes | world | standing | boots |
|---|---|---|---|---|
| cp-play r2 (integ9), reference r2-fps-scan6 | — | 25.7 (13.8) | 35.1 | 1 |
| cp-play r2 (integ9) | — (`pr-base1`) | 25.1 (15.3) | 25.1 | 1 |
| cp-play r2 (integ9) | all (`pr-k1`…`pr-k5`) | 19.8–28.1 (12.4–14.4) | 23–33 | 5 |
| **integ10** 49e83499 | — (`pr10-base`, `pr10-base2`) | **22.5 (18.5), 32.6 (17.3)** | 29.2, 35.4 | 2 |
| **integ10** | all (`pr10-lane2`, `pr10-lane3`) | **30.3 (17.5), 25.1 (16.7)** | 36.2, 27.6 | 2 |
| integ10 + 9d47b051 (scratch) | — (`pr10-basek6`, GPU 89 % busy from another guest) | 29.5 (19.1) | 30.7 | 1 |
| integ10 + 9d47b051 (scratch) | all (`pr10-k6`, quiet host) | **33.2 (21.3)** | 32.6 | 1 |
| play.p9 (10.5 min in the world), cp-play r2 reference r2-play-scan1 | — | 19.0 (11.7) | — | 1 |
| play.p9, cp-play r2 kernel | all (`pr-play1`) | 21.6 (13.6) | — | 1 |
| play.p9, **integ10** | all (`pr10-play`: save, quit status 0, e2fsck rc 0, game copy unchanged) | **23.3 (14.6)**, 372 rows; map 50.4 | — | 1 |

So: **the lane's changes are fps-neutral within the boot-to-boot spread**
(they were not on the critical path), the integ10 kernel lifts p10 from
12–15 to 17–19 and the median into 22–33, and the 30 fps median is reached
in 3 of the 5 integ10 boots — not reliably, and p10 stays at 17–21.

| measured (world, fpsprobe walk) | cp-play r2 kernel | integ10 (+ lane) | instrument |
|---|---|---|---|
| the game's two hottest threads | 81–92 % running each, ~25 % of wall in the kernel | 59–73 % + 18–31 % running, 11–14 % kernel | `[THRW]` (§2.3) |
| game threads READY (runnable, not on a CPU) | **1.0–1.8** | **0.24** | `[THRW]` |
| game threads running in the kernel | 1.4–1.9 of 2.6–2.9 | 1.0 of 2.4 | `[THRW]` |
| game threads in `select` (Wine's Sleep), in-kernel | 0.40–0.47 | 0.00–0.01 | `[THRW]` |
| wineserver on a vCPU | 40–56 %, ~95 % kernel | 24–28 % | `[THRW]` |
| host GPU busy | 27–30 % | 23–26 % (89 % when another guest used it) | `gpu_busy_percent` |
| host render-server ring thread (`vkr-ring`) | 36 % of a core | 29–35 % | `host-threads` |
| QEMU main thread | 10 % of a core | 9–13 % | `host-threads` |
| Venus replies waited for (sync calls) | 0.03 ms / frame | 0.03 ms / frame | `[HBV]` |
| app-thread `vkQueueSubmit` (25 per frame) | 9.3 → **5.8 ms / frame** (lane) | 5.3–5.9 ms | `[HBV]` |
| fenced `SUBMIT2` / unfenced / ring notify, per call | 72 / 73 / 360 µs → **12 / 11 / 11 µs** (lane) | 12 / 10 / 13 µs | `[HBV]` |

## 2. Instruments (all read-only, kept in the tree)

### 2.1 `[HBV]` — where Venus' own threads wait (Mesa, `tools/mesa/venus/hb_present_stats.h`)

Printed with every `[HBP]` row (same 2 s window, `HB_PRESENT_STATS_MS`).
Per entry `count/avg µs/total ms/max µs`: `ring`/`tls` (a reply waited for
on the instance / a thread's TLS ring), `space` (ring space), `slpR`/`slpO`
(`vn_relax` sleeps), `yield`, `ntf` (`vkNotifyRingMESA`), `subF`/`subU`
(fenced / unfenced `GPU_IOC_SUBMIT2`), `wait` (`hobbyos_wait`: fences and
semaphores), `fwait` (blocking `FENCE_WAIT2`), `qsub` (app-thread
`vkQueueSubmit*`), `pwait` (`vkWaitForPresent*KHR`); then the six command
types whose replies cost most (`cmd TYPE:n/avg/max`), and the six threads
that waited longest (`tid T:w=n/ms,q=n/ms,p=n/ms`).
`tools/cp/present/hbv_summary.py RUN_DIR [vn_protocol_driver_defines.h]`
turns them into per-frame numbers per phase.

### 2.2 host side (`tools/cp/present/hostprobe.sh RUN_DIR`)

Waits for `phase world` + 25 s and records, for the run's own QEMU and its
render-server processes only: per-thread CPU and context switches
(`host-threads.txt`), KVM exits / MMIO / PIO / IRQ injections by bpftrace
(`host-kvm.txt`), `gpu_busy_percent` of the 9070 XT (`host-gpu.txt`) and a
`perf record` of the host threads (guest time is invisible to it).

### 2.3 `[THRW]` — what every guest thread is doing (kernel, `process.c`)

Every 10 ms PIT tick on the boot core, every Linux-ABI thread is classed:
running (and then: in a syscall — which —, or in the kernel for a fault or an
interrupt, or in user mode), READY (runnable, not on a CPU), in a futex, or
blocked — in which syscall.  Every 5 s one row per busy group:

```
[THRW] pid=92 ticks=500 run=2.62 ready=1.01 futex=27.27 blk 0:12.08 23:3.88 ...
       kern=1.49 trap=… ksys 23:0.43 16:0.36 0:0.20 202:0.15 228:0.14 ...
       top 92:91/1/25/…(…) 96:91/1/24/… 121:26/0/… Cyberpunk2077.e
```

`run/ready/futex/kern` are average thread counts; `top` lists the group's
three busiest threads as `tid:run%/ready%/kern%/trap%(syscall=%…)`.

## 3. What was ruled out, with numbers

### 3.1 The host is not saturated (`pr-base1`, `host-*.txt`)

12 s in the world: QEMU main thread 10 % of a core (egl-headless' readback,
the control queue, fence polls), `vkr-ring-11` 36 % (Venus' command decoding:
every `vkCmd*` of every command buffer is replayed into RADV on this one
thread), the two `vkr-queue` threads < 1 %, GPU busy 27–30 % (p90 36 %).
Nothing waits on a host resource that is full.

### 3.2 The Venus transport was slow per call, but not the frame's limit

`[HBV]`, world, per frame (≈ 25 frames/s), baseline (`pr-hbv1`):

| | n / frame | ms / frame | per call |
|---|---|---|---|
| app `vkQueueSubmit` | 24.7 | 9.3 | 377 µs |
| fenced `SUBMIT2` ioctl | 24.8 | 1.8 | 72 µs |
| unfenced `SUBMIT2` (venus' `vkSubmitVirtqueueSeqnoMESA` after every submit + ring notifies) | 27.9 | 2.0 | 73 µs |
| ring notify | 3.1 | 1.1 | 360 µs |
| renderer waits (all threads) | 58.7 | 22.0 | 375 µs |
| replies (sync calls) | ~0.6 | 0.03 | — |

Why a 16-byte ioctl took 70–360 µs (`host-kvm.txt`): QEMU 10.2.1 defaults
`ioeventfd=off` for virtio-gpu, so every guest notify is a userspace MMIO
exit (117 k/12 s) that takes the BQL in the vCPU thread, while the main
thread holds the BQL for egl-headless' ~3 ms readback of each flip; and the
guest kernel did all of it under `gpu_lock` with interrupts off.  The
unfenced submits additionally spun until QEMU had processed them.  The flip
held `gpu_lock` (IRQs off) for its whole 3 ms, so any app submit, fence
kick-poll or GPU interrupt on another vCPU spun behind it.

Fixes (§4), each A/B'd: per call 72 → 12 µs (fenced), 73 → 11 µs
(unfenced), 360 → 11 µs (notify); app `vkQueueSubmit` 9.3 → 5.8 ms per
frame; present `sub` 6.3 → 4.4 ms.  **World fps: level** (table §5).

### 3.3 Present: not the limit

The flip (QEMU's readback) is 3.0 ms and the present thread's `sub` 4–6 ms,
both off the game's threads (venus' async present).  `vkWaitForPresentKHR`
is never called by the game's vkd3d-proton (`pwait` 0).  Earlier round 2
data say the same: scanout and put_image are level over the same 6-minute
segment (`docs/cp-p9.md` §10.4).

## 4. Changes

1. **`tools/cp/host-venus.sh`: `ioeventfd=on` on `virtio-gpu-gl-pci`**
   (`VENUS_IOEVENTFD=0` restores QEMU's default; `VENUS_GPU_OPTS` adds
   properties for A/Bs).  Userspace MMIO exits 117 k → 18 k per 12 s.
2. **Kernel, `virtio_gpu_3d.c`: small unfenced `SUBMIT2` streams are posted**
   on an async slot and not waited for (`GPU3D_KIND_POSTED`), as Linux
   virtgpu's EXECBUFFER returns before the host has run it.  The virtio
   queue keeps them ahead of everything posted later.  Host errors on them
   are logged (`[VENUS] posted submit error`), not returned.  Streams over
   256 bytes keep the synchronous path.
3. **Kernel, `virtio_gpu_x64.c`: the scanout flip no longer holds
   `gpu_lock` across QEMU's readback.**  `SET_SCANOUT_BLOB` and
   `RESOURCE_FLUSH` go on two async slots (`GPU3D_KIND_WAITED`, own request
   buffers) and the flipper waits with the lock dropped and interrupts on;
   the ioctl still returns only after the flush (present-id semantics
   unchanged).  One flip in the queue at a time; no free slots → the P7
   path.  `[P7] scanout flip unlocked async/sync` reports the path once.
4. **Instruments**: `[HBV]` (Mesa), `[THRW]` (kernel), `hostprobe.sh`,
   `hbv_summary.py`, `fpsprobe-stack.p9`.

## 5. Runs (strix, `-smp 8 -m 32G`, Venus; median (p10) fps of winevulkan `trace:fps`)

### 5.0 On the cp-play r2 kernel (integ9 + cp-play), before the integ10 merge

fpsprobe (`tools/cp/p9/fpsprobe.p9`: load ManualSave-86, 90 s of scripted
walking = "world", then 20 s standing = "prof").  The walk is the same every
boot; the spread between boots of one configuration is ±4 fps (host load:
1–3 other lanes' guests).

| run | configuration (kernel: cp-play r2 unless noted) | world | standing | other guests |
|---|---|---|---|---|
| r2-fps-scan6 (cp-play, reference) | integ9 + cp-play, play-venus Mesa | 25.7 (13.8) | 35.1 | 2 |
| **pr-base1** | this lane's tree before any change (= cp-play), play-venus Mesa, `ioeventfd` off | **25.1 (15.3)** | 25.1 | 2 |
| pr-hbv1 | + `[HBV]` Mesa | 26.5 (16.4) | 28.0 | 2 |
| pr-ioe1 | + `ioeventfd=on` | 25.9 (14.7) | 28.0 | 3 |
| pr-k1 | + posted unfenced submits + unlocked flip (kernel) | 26.0 (14.4) | 26.5 | 2 |
| pr-k2 | + `[THRW]` | 28.1 (14.4) | 33.1 | 2 |
| pr-k3 | + `[THRW]` kernel share | 20.0 (12.4) | 23.1 | 1 |
| pr-k4 | + per-thread syscall split | 19.8 (13.8) | 23.2 | 1 |
| pr-k5 | + trap split, stack profile (`fpsprobe-stack.p9`) | 24.0 (12.5) | 32.0 | 1 |
| **pr-play1** | all of the lane, play.p9 (Gate boot A: LOAD → 10.5 min in the world → map ×2 → F5 QuickSave-0 → QUIT, status 0, e2fsck rc 0, game copy unchanged) | **21.6 (13.6)**, 377 rows; map 48.7 | — | 0 |
| r2-play-scan1 (cp-play, reference) | play.p9 | 19.0 (11.7), 366 rows | — | 2 |

The integ10 re-baseline (§1 table, `docs/cp-present-evidence/integ10-rebaseline-strix.txt`)
used scratch branches: `present-base10` (= integ10), `present-base10k6`
(integ10 + 9d47b051), `cp-present` (integ10 merged, f131f594), `present-k6`
(cp-present + 9d47b051).  Trap: `rsync -a` keeps the Mac's mtimes, so
after switching branches the strix objects can be newer than the sources;
`pr10-lane-stalekernel` ran integ10's kernel objects with the lane's Mesa
(21.3 (14.4)).  Every later boot touched `src/kernel` + `src/include` first.

`[HBV]` per frame, world (`docs/cp-present-evidence/hbv-*.txt`):

| per frame | pr-hbv1 (before) | pr-ioe1 (+ ioeventfd) | pr-k1 (+ kernel) |
|---|---|---|---|
| app `vkQueueSubmit` | 24.7 / 9.31 ms | 24.8 / 6.75 ms | 24.7 / 5.84 ms |
| fenced `SUBMIT2` | 24.8 / 1.78 ms | 24.9 / 0.82 ms | 24.9 / 0.49 ms |
| unfenced `SUBMIT2` | 27.9 / 2.04 ms | 28.1 / 2.03 ms | 28.0 / 0.32 ms |
| ring notify | 3.1 / 1.12 ms | 3.2 / 0.88 ms | 3.1 / 0.04 ms |
| renderer waits (all threads) | 58.7 / 22.0 ms | 58.9 / 16.3 ms | 58.8 / 16.9 ms |
| present `sub` / flip (`[HBP]`) | 6.33 / 4.33 ms | 4.49 / 3.00 ms | 4.63 / 2.95 ms |
| world fps | 26.5 | 25.9 | 26.0 |

### 5.1 Per-frame time, world, by thread

cp-play r2 kernel (~38 ms per frame at 26 fps):

| thread (pr-k1/k4, world) | per frame | source |
|---|---|---|
| game main thread (tid 92) | **~32 ms on a vCPU** (81–91 %), ~9.5 ms of it in the kernel, 0.4–1 ms READY | `[THRW]` |
| game worker (tid 96) | ~32 ms on a vCPU (83–92 %), ~9 ms kernel | `[THRW]` |
| vkd3d-proton submit thread (123) | 15–17 `vkQueueSubmit`, 0.7–0.8 ms | `[HBV]` tid |
| vkd3d-proton fence waiter (122) | 40 waits, 6.6–7.6 ms waiting for the GPU timeline | `[HBV]` tid |
| frame-fence thread (163) | 1 wait, 3.6–4.1 ms | `[HBV]` tid |
| venus present thread (166) | 4 submits + waits, 3.7–4.9 ms (blit + its fence) | `[HBV]` tid, `[HBP]` sub |
| WSI queue thread | 1 flip, 3.0 ms (QEMU's egl-headless readback inside `RESOURCE_FLUSH`) | `[HBP]` flip |
| wineserver | 40–56 % of a vCPU, ~95 % of it in the kernel (`epoll_wait`, `read`, `write`) | `[THRW]` |
| host: GPU / `vkr-ring` decode / QEMU main | ~11 / ~14 / ~4 ms busy per frame | `host-*.txt` |

integ10 + lane (+ 9d47b051 in `pr10-k6`, ~30–33 ms per frame at 30–33 fps):
the two hottest game threads 59–73 % and 18–31 % on a vCPU (11–14 % / 4–6 %
of wall in the kernel), READY 0.24 threads for the whole game, fence waiter
40 waits / 7.8 ms, frame-fence thread 1 wait / 4.0 ms, present thread 4.2 ms,
flip 3.0 ms, app `vkQueueSubmit` 5.3 ms (`thrw-integ10-strix.txt`).

## 6. What the frame waits for — and who owns it

1. **The game's own two hottest threads (CPU)** (cp-play r2 kernel; on
   integ10 the same threads are 59–73 % and 18–31 % busy, 11–14 % kernel).
   The main thread and one worker are each on a vCPU 81–92 % of the time; the frame rate follows
   their work.  A quarter of the wall time of each (≈ 1/3 of their CPU
   time) is HobbyOS kernel time, spread over many syscalls (no single one
   above 3 % of wall time: `clock_gettime` 1–7 %, `futex`, `rt_sigprocmask`,
   `write`, `read`), i.e. the cost of every trap, not one hot call.  Group-wide,
   1.4–1.9 of the 2.6–2.9 running game threads are in the kernel.
   Kernel PC samples: `spin_acquire_raw` 7–10 % (callers `fs_poll_park`,
   `schedule`, `vm_touch`: `proc_lock`/`vm_lock`), `linux_compat_dispatch`
   2.4 %, `sched_wake_idle_cpu` 2 %.  **Owner: C1-k6** (trap/syscall path,
   `proc_lock`).  Not touched here.
2. **Runnable threads that wait for a CPU.**  1.0–1.8 game threads are READY
   on average while 3.5–4 vCPUs halt; single timer-woken threads (the P8
   keep-alive, game threads in `select` = Wine's `Sleep`) are READY 15–30 %
   of their time.  With ~18 k reschedule IPIs/s (`host-kvm.txt`: vector
   0x81) part of this is plain wake latency; part may be wakes no CPU picks
   up until the next 10 ms tick.  **Owner: C1-k6** (scheduler wake policy).
   **Status: integ10's C1-k6 round 2 (sync wakes, READY hint, keyed poll,
   event-driven epoll) took READY from 1.0–1.8 to 0.24 threads and the
   in-kernel `select` time from 0.4 to 0 threads** (`thrw-integ10-strix.txt`);
   9d47b051 (halts bounded to 1 ms) gave the best boot (33.2 (21.3)).
3. **Wine's synchronisation goes through the wineserver.**  Wine 11 without
   ntsync/esync: every Win32 wait/set is a wineserver round trip; 12 game
   threads sit in `read()` on their server pipes, and the wineserver itself
   runs 40–56 % of a vCPU, almost all of it kernel time.  A faster trap
   path (1) helps it directly.
4. **Not the limit (measured):** the present path (flip 3 ms + present
   `sub` 4–5 ms, both off the game threads; `vkWaitForPresentKHR` unused),
   the Venus transport after §4 (ioctls 11–12 µs), synchronous Venus
   replies (0.03 ms/frame), the host GPU (29 % busy), the host decoder
   thread (36 %), QEMU's main thread (10 %).

What would make 30 reliable (integ10 is at 22–33 median, p10 17–21): the
kernel share of the game's hot threads (11–14 % of wall on integ10) and
of the wineserver is still trap/syscall-path work (C1-k6); the p10 dips are
streaming phases of the walk.  Fewer wakeups per Win32 wait (an ntsync-like
`/dev/ntsync` or esync eventfds would take the wineserver out of the
game's waits — a Wine-build + kernel item for the integrator to assign).

## 7. Commands

```
# Mesa with [HBV] (this tree's build.sh)
VENUS_WORK=$HOME/cp/lanes/present-venus bash tools/mesa/venus/build.sh all
# one fpsprobe boot + the host probe (no QEMU change beyond host-venus.sh's defaults)
P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=pr-k1 VENUS_BUILD=$HOME/cp/lanes/present-venus \
  P8_EXPORT=/srv/nfs/cp-present-p9 PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 \
  bash tools/cp/p9/run-p9.sh &
bash tools/cp/present/hostprobe.sh $PWD/obj/cp-p9/pr-k1 25 12
# baseline A/B knobs: VENUS_IOEVENTFD=0 (QEMU's default), VENUS_BUILD=~/cp/lanes/play-venus
python3 -I tools/cp/present/hbv_summary.py obj/cp-p9/pr-k1 \
  $HOME/cp/lanes/present-venus/src/mesa-26.2.6/src/virtio/venus-protocol/vn_protocol_driver_defines.h
python3 -I tools/cp/p9/prof.py obj/cp-p9/pr-k5/prof-fp-world.txt hobbyos.elf
```

## 8. Evidence files (`docs/cp-present-evidence/`)

| file | what |
|---|---|
| `host-pr-base1-strix.txt`, `host-pr-ioe1-strix.txt`, `host-pr-k1-strix.txt` | host threads / KVM events / GPU busy, cp-play r2 kernel: before, + ioeventfd, + kernel changes |
| `host-pr10-base-strix.txt`, `host-pr10-lane2-strix.txt`, `host-pr10-k6-strix.txt` | the same on integ10, integ10 + lane, + 9d47b051 |
| `hbv-pr-hbv1-strix.txt`, `hbv-pr-ioe1-strix.txt`, `hbv-pr-k1-strix.txt` | `[HBV]` per phase + raw world rows |
| `thrw-strix.txt`, `thrw-integ10-strix.txt` | `[THRW]` per phase (cp-play r2 kernel; integ10) |
| `prof-pr-k5-stack-strix.txt` | guest PC profile with kernel stack words, world |
| `integ10-rebaseline-strix.txt` | every integ10 fpsprobe boot, kernel-labelled |
| `play-pr-play1-strix.txt`, `play-pr10-play-strix.txt` | play.p9 on the cp-play r2 kernel and on integ10, both with the lane |
| `gates-strix.txt` | the gate batch (§9) |
