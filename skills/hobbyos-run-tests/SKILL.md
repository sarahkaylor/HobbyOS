---
name: hobbyos-run-tests
description: "Use when running or interpreting HobbyOS tests. Covers the three tiers — host golden tests, kernel unit tests, userland integration tests — plus the two-VM RDMA test, pass/fail signals, and the deadlock timeout rule."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, testing, unit-tests, integration, host-tests, qemu, rdma]
    related_skills: [hobbyos-build-and-run, hobbyos-gui-test, hobbyos-kernel-constraints, hobbyos-proxmox-gpu]
---

# HobbyOS: Running & Interpreting Tests

## Overview

HobbyOS has three test tiers, fastest to slowest: **host golden tests** (userland logic compiled natively on macOS, no QEMU), **kernel unit tests** (subsystems checked in EL1 before the scheduler), and **userland integration tests** (real processes/syscalls/SMP/IPC under the booted kernel). Plus a **two-VM RDMA test** for the remote-PCIe path. Start at the cheapest tier that can catch the bug: reach for host tests for pure logic, unit tests for kernel internals, integration for anything touching processes or drivers. Every tier signals pass/fail in text and must halt QEMU itself — a run that just *hangs* is the failure.

## When to Use

- Running the suite after a change, or adding a new test.
- Deciding which tier fits what you changed.
- Interpreting a hang/timeout vs. a clean fail.

Don't use for: choosing build ARCH/MODE mechanics (→ [[hobbyos-build-and-run]]) or GUI validation (→ [[hobbyos-gui-test]]).

## Tier 1 — Host golden tests (seconds, no QEMU)

Userland programs are compiled natively on macOS with `-DHOST_TEST` against a mock libc/framebuffer (`src/host/compat.c`), then run directly. This is the fast baseline for game/app/logic bugs.

```bash
make host_tests    # builds + runs the editor and pong host tests
```

Pattern (see `src/host/pong_test.c`): the test `#include`s the userland `.c` with `#define main <name>_main`, then unit-tests its pure functions (ball physics, collision, scoring…) with `ASSERT`s, printing `PASS`/`FAIL` and a summary, returning non-zero on any failure. Add a new one by writing `src/host/<prog>_test.c`, adding a `*_host` link rule in the Makefile's host section, and listing it under the `host_tests` target. **Prefer this tier for any logic that doesn't need real kernel services** — it's the fastest signal by far.

## Tier 2 — Kernel unit tests (in-EL1, before scheduler)

```bash
./run_unit_tests.sh          # ARM: builds, runs headless, greps, 300s verdict budget
./run_unit_tests_intel.sh    # x86_64
# or directly:
make unit_tests              # ARCH=arm; make unit_tests_intel for x86_64
```

`run_unit_tests.sh` runs `make unit_tests` headless into `qemu.log` and polls for the signal strings, killing QEMU on either:

- **`UNIT TESTS PASSED`** → exit 0.
- **`UNIT TESTS FAILED`** → exit 1, dumps the log.
- Neither within the budget → "timed out," exit 1. The budget is `UNIT_TESTS_TIMEOUT`, default **300 s** since the 1 GiB-disk era (fat16 unit suites legitimately take ~60-90 s; the old hardcoded 20 s reds green suites). The build phase is deliberately unbounded — only the QEMU run is timed.

## Tier 3 — Userland integration tests (booted kernel)

```bash
make test          # ARCH=arm;  make test_intel for x86_64
```

`MODE=test` makes the scheduler auto-spawn a fixed sequence of userland test binaries (`fork_test`, `smp_test`, `pipe_test`, …) to validate syscalls, multi-core scheduling, and IPC, then halt. Use when the change touches processes, scheduling, the filesystem, or drivers — things the host tier can't model.

The x86_64 `QEMU_CMD` does **not** pass `-enable-kvm`, so on a host with `/dev/kvm` the suite still runs under TCG and takes 5-10x longer (an hour instead of ~15 min — easily mistaken for a hang; check for `[WATCHDOG] console silent` spam and zero `PASS` lines before concluding it's stuck). Pass it through the existing `QEMU_ARGS` hook:

```bash
make test_intel QEMU_ARGS=-enable-kvm
```

Authoritative liveness check for a long run: `wc -l` the log twice a minute apart — if it grows (even with watchdog/`CDATA` noise) it is alive; if it is flat for minutes with no new `PASS`/`FAIL`, treat it as the hang it is.

## Two-VM RDMA test (remote-PCIe, x86_64)

```bash
./run_two_instances.sh
```

Boots two x86_64 instances locally — a **host/provider** (owns the `-device edu` card, listens on `:12345`) and a **receiver/consumer** (connects over a QEMU socket netdev, emulates the card via RDMA) — using `fw_cfg opt/pcishare=host|guest:0x1234:0x11e8`. It polls `receiver.log` for `UNIT TESTS PASSED/FAILED` with a 25 s timeout and prints both logs. This is the local rehearsal for the Proxmox GPU path in [[hobbyos-proxmox-gpu]].

## x86 tier: expect instability, run it as a bounded battery

**The x64 integration tier at current tip does NOT complete.** On the CI VM
(batch of 8-16 parallel copies, 64 vCPU/128G) every run reboot-loops the guest
(silent triple fault → OVMF reboot; 10-40+ boots/run) with LOCKFOREVER +
WATCHDOG + IDLESTUCK storms; zero runs ever print `System halt.`. A harness
that runs `make test_intel` and waits will hang FOREVER (guest resets instead
of powering off; `-action shutdown=poweroff` does not stop a reset storm) —
always bound it, e.g. the `x64batch.sh` pattern: seed N copies from ONE
prebuilt template (rsync source + `cp -a` obj/ disk.img hobbyos.elf per
copy), run each in its own dir (own disk.img ⇒ no QEMU write-lock collision),
`fuser -k <copy>/disk.img` to kill, `timeout`/tmux to bound. Build the
template with `make ARCH=intel MODE=test hobbyos.elf disk.img` (NO `run`).

**ARM's green ≠ x86's green — and that's platform, not arch.** ARM runs
`-accel tcg,thread=multi` (MTTCG cooperative threads); x86 runs `-enable-kvm`
(TRUE parallel vCPUs). SMP races that fire constantly under KVM may never
reproduce under MTTCG — the "Intel side only" pattern is expected; do not
infer the ARM code is better for it, and never claim an x64 lock fix without
re-running the KVM battery.

**QEMU_ARGS pitfalls:** `make test_intel QEMU_ARGS='-d cpu_reset -D ...'` —
GNU make parses `-d`/`-D` AS MAKE FLAGS even after goals (it dumps its
database and exits 2). For `-d`-style QEMU tracing, invoke qemu directly or
pass only `-qmp unix:...,server,nowait -monitor none` (safe). Live forensics
on a wedged guest: `info registers -a` + `xp/8gx 0x74c18f40` (lock_wait_addr),
`xp/8gx 0x74c18f80` (lock_wait_caller), `xp/8gx 0x74c18fc0` (caller2; kernel
BSS is identity-mapped so virtual == guest-physical).

## x64 fix status (2026-09-27)

A full fix set landed (commit fabeff8, then 8aa4cc0): single console lock
(lock-free raw diagnostics), kernel-mode faults dumped to COM1 (were
dead-silent on 0xE9), CS/SS carried through the process context[34]/[35]
(a user process preempted inside a kernel busy-wait was resumed with user
selectors over a kernel RIP -> triple fault), no preemption of a user
process in kernel mode (timer AND the 0x81 reschedule IPI both guard),
schedule() runs IRQ-off across the switch, frame-carried user RSP, and a
lost-owner reaper.  Metrics before: 212/212 boots failed, 1525 LOCKFOREVER;
after: 7/8 parallel KVM copies lock-free, wave reaches STRESS.BIN which
passes, ~1 rare single-CPU resume corruption per 10-20 boots remains.

## ARM on the test server: launch QEMU directly, NOT via tmux -d

On the hobytest VM an ARM `tmux new-session -d ... qemu-system-aarch64`
run comes up in state T (SIGSTOP'd) with zero CPU time and zero serial
output; the equivalent x64 runs work fine in tmux.  Launch ARM QEMU with
`nohup ... > log 2>&1 &` instead.  At the 2026-10 tip, `MODE=test` does **not** run the kernel unit tests inline (main.c ~454: only `KERNEL_MODE_UNIT_TEST` calls `run_all_unit_tests()`; `MODE=test` goes straight to the test-wave loader) — run the unit tier as its own boot; a red unit tier no longer blocks the wave.

**Fresh-VM prerequisites:** the ICU fetch overlay needs `unzip` (plus
`tar`/`xz`), and since Wave 1g `disk.img` depends on the ICU + SQLite vendor
builds — so every tier that rebuilds the disk, including the unit tiers via
their `fresh_disk` step, pays both builds (budget walls accordingly).  A tier
dying at ~90 s with `fetch.sh: unzip: command not found` + `Error 127` on
`obj/<arch>/icu/MANIFEST.txt` is a missing host tool, not a test failure:
`sudo apt-get install -y unzip` (hobytest has NOPASSWD sudo).

**After any sync that touched committed `third_party/` files, run
`git checkout -- third_party/` on the VM source tree before the next tier.**
The tier runner copies `~/HobbyOS` wholesale into each fresh workdir
(`third_party/` included), but a source rsync that excludes `third_party/`
leaves the working tree on the OLD vendor scripts — a fresh-workdir tier then
builds with stale vendor logic (seen: ICU `build-target.sh` without
`-mcmodel=large` -> `unknown endianness` in unit-x64 while every other tier
was green).  `fetch.sh` self-heals the extracted `src/` per workdir from
committed tarballs; committed scripts/patches need the checkout.  Two more
sync-layer rules: (a) when SCRIPTING a battery, print the completion-marker
presence AND the FAIL-token count per tier — an rc-only driver reports green
on a wave that contains FAIL tokens; (b) after a USER_CFLAGS/code-model flip,
wipe stale `obj/<arch>/*.o` once — make cannot detect flag changes and will
relink small-model objects into the flipped link (seen: a pre-flip
`obj/intel/sqlite3.o` breaking `sqltest` while the Makefile rule was
correct).

## Unit re-runs need a fresh disk (stateful fat16 suite)

fat16 unit tests CREATE/MOVE files on disk.img, so a second
`./run_unit_tests*.sh` in the same tree without a rebuild false-fails
`fat16_rename("/MOVESUB", "/MOVEDIR2/MOVESUB")` (fat16_test.c:126) —
a stateful-disk artifact, not a code regression.  The unit scripts now run
`make fresh_disk` (touches the kernel so the disk.img recipe re-runs its
dd+mkfs — no deletions).  On older trees without that target, do
`touch src/kernel/main.c` before the run.  CI is immune (fresh copy per
tier, fresh disk per copy).

## fat16 size assertions rot silently

`fat16_test.c` hardcodes on-disk binary sizes ("/UNEXPAND_T.BIN" file_size
12592).  A userland change to the tool silently makes the constant stale
(12896) and fails the ARM unit stage, blocking every downstream run.  When
an ARM/x64 unit run dies on `file_size` / `EXPECT_EQ` in fat16_test, stat
the built `obj/<arch>/<tool>_test.bin` and update the constant to the
actual size.

Same class hits **disk-geometry bounds**: `time_test.c`'s `test_fat16_stats` asserted `total < 256 MiB`
(comment: "64 MB image") and went stale when the startup disk grew to 1 GiB (Oct 2026) — a deterministic
red on every unit run (`EXPECT_EQ FAILED: (total < 256ULL*1024*1024) == 1`, ~1008 MiB actual).  After
any `disk.img` size change, grep the kernel test suites for size bounds and widen them to cover the
new geometry.

## The Deadlock Timeout Rule (project-wide)

**If any test tier runs longer than ~30 s without a clear PASS/FAIL, stop it and treat the slowness as the defect** — almost always a deadlock, an unhandled trap/fault, or a hung driver waiting on an interrupt that never fires. Hangs frequently emit **no** failure log, so "no output yet" is itself the signal. Do not paper over it by bumping the timeout; find the root cause (see [[hobbyos-kernel-constraints]] for the common trap sources). The wrapper scripts already cap at 20–25 s for this reason.

## A wave suite can truncate silently — count check NAMES, not tokens

A suite can die mid-run with **zero FAIL tokens**: CXX_T died after ~9/20 checks in
about half of wave runs when `SYS_THREAD_CREATE` returned `-EAGAIN` (all 64 PCB slots
momentarily live — forensic showed `used=63/63 done=0`).  libc++ `std::thread` is built
`-fno-exceptions`, so an EAGAIN from `pthread_create` **aborts the process** instead of
retrying like C callers do — the suite just stops, and a FAIL-token scan reads clean.
The only tell: count DISTINCT check names per suite and watch the all-suite
`ALL TESTS PASSED SUCCESSFULLY` count (10 in a full wave; 9 means one suite never
finished).  Root cause is fixed at the shim (pthread_create retries EAGAIN ~2 s
internally; sustained exhaustion still returns EAGAIN) and the kernel's no-slot path
now prints a rate-limited `[KERNEL] thread_create: no free slot (used=N/63 done=M) for
pid=P` forensic (first three events per boot).  Measured: pre-fix 6/13 wave runs died,
post-fix 0/12.  If a future thread-bearing test truncates anyway, that print tells you
whether slot pressure was the cause.

**Launch-hungry CHECKS (not just whole suites) share this brittleness.**
Under sustained slot pressure a check whose `pthread_create` returns EAGAIN
past the shim's retry window fails even though the kernel is correct — seen:
CXX_T `condvar_wait_notify` (documented ~1/30, also hits CI waves) and THRD_T
`futex-wake` (waiter create EAGAIN -> the wake finds no waiter -> `woke == 0`
-> FAIL; forensics `no free slot (used=63/63 done=0)` in the same log).
Before chasing any wave FAIL in those checks, grep the log for the `no free
slot` forensic: if present it is this documented class — the fix belongs on
the TEST side (retry / skip-with-note on sustained EAGAIN), not in the
kernel, and the wave should be re-run for a clean receipt.  In CI conditions
the class fired on ~2/3 consecutive waves; locally ~0.

**The slot-famine WEDGE is an amplifier defect, not a test defect — fix
pacing, don't rerun.**  When the full wave + TORTURE fills the 63-slot PCB
table (61+ wave programs load back-to-back, so the table is legitimately
full at wave start), the pre-fix retry machinery wedged a whole soak at 1
round: the loader retried `process_create` every 100 ms up to 18 000 times
(~30 min) per spawn, and every failed attempt printed an UNTHROTTLED line
(observed: 138k 'no free process slots' + 145k 'acquiring lock' lines in
one 35-min run — zero process exits for the middle 25 min, because the
print storm saturated the UART and stalled the exits that free slots).
Three rules for this class: (1) the loader/spawn retry loop must distinguish
a DRAINING transient (slots free continuously; wait it out, budgeted) from
a STUCK table (no slot freed for a stall window; bail early) and must never
outlast the transient by orders of magnitude; (2) slot-pressure forensics
and per-call process_create trace prints must be rate-limited (first 2-4
events) — the console storm IS the amplifier; (3) when calibrating the
budget, a fail-fast 2 s child budget breaks the wave start (measured 8/8
prior waves green -> 9 FAIL tokens: IPC_T fork-dependent fd checks have NO
fork-EAGAIN handling and shell_test2/3 spawns fail), so ride out the
transient generously.  The stuck-table detector (no slot freed for ~2 s)
must give up early ONLY for CHILD spawns: a BOOT load (caller_pid < 0)
has no caller to retry, and bailing silently DROPS the wave program —
observed: TORTURE's final-wave boot load bailed and MODE=soak ran as a
plain wave (all suites green, clean System halt, zero [SOAK] rounds, rc 0
— an invalid soak that looks like a pass).  Verify a MODE=soak run
actually soaked (grep for '[SOAK] round=') before trusting it.
The wave can be momentarily 63/63 for tens of seconds while ~60 programs
load and drain; spawns must succeed there.  If a wave FAIL token appears
with the rate-limited 'no free' forensic present only 1-6 times, it is the
transient class, not a regression.

**A separate intermittent class: IDLESTUCK lost-owner freeze.**  With the
famine fixed, soaks can now run far enough to hit a rare lost-wake race
(2/3 of post-fix soak attempts froze at rounds 186 and 337; the base's
806-round clean run had IDLESTUCK=0): TORTURE's exec-child slot is
PROC_STATE_RUNNING and still CLAIMED by a CPU that is itself IDLING; the
diagnostics only reclaim RUNNING slots with NO claiming CPU, so the stale
claim pins the slot forever, everything else drains to THREAD_DONE, rounds
stop permanently (no System halt; run burns the 35-min timeout).  Tell:
round count frozen + '[IDLESTUCK] slot=N pid=N st=3' with a claim that
names the idling CPU + '[IDLESTUCK] claims: c3=1 ...'.  This is a
scheduler/claim-class bug, not famine (violations stay 0, floods stay
throttled).  RESOLVED 2026-10-02 (class-B lane; docs/browser/l8-idlestuck-class-RESOLVED-l8-idlefix.md): root cause = the claiming CPU wedges permanently IRQ-off inside the claim->resume window (heartbeat freezes).  Re-READY-ing the slot FAILED empirically (re-picking the wedged child cascades and wedges all 8 cores - the poison rides the child's AS).  The working recovery: LOSTWAKE disposes a RUNNING slot whose EVERY claimer heartbeat is stale (>=2000 ms) when the table is otherwise DRAINED - release the zombie claims + group_teardown code 97, so the parked WAIT_CHILD parent reaps a rejected exec and reforks fresh; rounds continue, violations 0.  Residual: each wedge permanently burns one core (~2/3 of soaks hit one; a 28-min soak loses ~1 core and still completes); clean-run receipts are luck-dependent.

**Counting method that survives splices:** count each suite's checks by per-name
SUBSTRING existence against its known name list (`grep -c <name>` one name at a
time), never a strict `name + ' : ' + PASS` adjacency regex — console splices
merge other processes' fragments into the verdict line, so the strict pattern
silently undercounts (measured: the same THRD_T log read 18/20 by regex and
20/20 by substring).  Cross-check with the suite's `ALL TESTS PASSED
SUCCESSFULLY` summary (the baseline grows as suites land — compare against a
recorded baseline at the SAME tip, not a stale number; a full ARM wave at the
Wave-1f+ICU tip logs 13 plain summaries plus the `[STRESS TEST]` line) — each
summary riding right behind that suite's last `: PASS` line bounds how much of
the suite ran.

**Setup guards are silent on success — fewer lines than call sites is normal.**
Suites guard their body with `if (setup(...) != 0) { check("<name> setup", 0);
return; }`: that check name prints ONLY on failure.  Expect fewer logged checks
than source `check("` call sites (measured: IPC_T = 82 sites, 12 failure-only
guards, 70 checks on a green run).  Before calling a suite truncated, extract
its source site list (strip `#ifdef HOST_TEST` regions), match names against the
log, and confirm every unmatched name is a `!= 0` guard.

## Scratch trees, CI runners, and wave attribution (Oct 2026)

- **/tmp ENOSPC with blocks free = inode exhaustion.** This workstation's
  /tmp is tmpfs with a ~1M-inode cap (`df -i /tmp`); several rsync'd trees
  (~200-400k files each) exhaust it. Symptoms: `rsync: mkstemp ... No space
  left on device` and `make` dying mid-suite when shell redirections fail.
  Fix: prune stale `/tmp/*-os-scratch`, `/tmp/*-wt` trees — but FIRST check
  `git worktree list` in both `~/Documents/GitHub/HobbyOS` and
  `~/webkit-hobbyos/HobbyOS`; some /tmp dirs are registered worktrees.
  (Per-run disks and temps inside the scratch dirs are now cleaned by the
  runners themselves — Oct 2026; only the trees remain manual.) Also:
  never write a *guard* file (via `tee`) in the same command that then reads
  it — under ENOSPC the tee fails, an empty guard silently disables the
  check, and a registered worktree can get deleted; `git worktree prune`
  cleans the stale registration.
- **Fork browser-lane runners** (`~/webkit-hobbyos/HobbyOS/continuation/`):
  `wk3/scripts/run_wk3.sh <run> <os_scratch>` (headless 86 MB WebProcess;
  needs `FW=~/.local/share/AAVMF/AAVMF_CODE.fd` and
  `LLVM_OBJCOPY=/usr/bin/llvm-objcopy`; 300 s stability window ⇒ full run
  ≈6 min, budget TIMEOUT_QEMU≥600) and `wk5/scripts/run_wk5.sh` (windowed
  acceptance via QMP; self-resolves FW; ~1 min session + build). Both BUILD
  the kernel from `<os_scratch>` themselves. Grep markers: WK-3 `WK3 JS-OK`,
  `WK3 FPC ... checksum=`, `WK3 ALL-DONE`, `[IMGPRF] done:`; WK-5
  `[WIN] exit rc=0`, `[WIN] load-ok ms=`.  All runners now remove their
  per-run artifacts at exit (the `disk-*-<run>.img`, 128 MiB each, plus
  fixed-path temps and the WK-5 QMP socket); `KEEP_DISK=1` retains the
  disk when a follow-up run (e.g. the timed `wk3_timed2.sh` harness)
  needs to re-boot it.  Scratch trees and fork-dir evidence are never
  touched.  Kernel-mode detection:
  `grep -qa -e 'Mode: WEBPROC' <elf>` directly — never `strings <elf> | grep
  -q` under `set -o pipefail` (SIGPIPE 141 false-negatives); a repo-root
  `hobbyos.elf` can be MODE=desktop.
- **Timed A/B pattern:** boot the assembled disk directly under
  `qemu-system-aarch64` and poll the serial file from QEMU start, printing
  wall-clock at marker hits (script: `~/.hermes/cache/scratch/l9-prefetch/`
  `wk3_timed2.sh`). `WK3_STABLE` env is not honored — the stability window
  is the driver default (~300 s); for timing, kill QEMU at the marker.
- **Wave FAIL attribution:** before blaming a change, run `test-arm` on a
  PRISTINE main copy in the same window. Same failure FORMS + same forensics
  (`no free`×6, `thread_create: no free`×3) on both trees = tip-ambient
  flake, not the lane. Tip-era forms (2026-10-04, all four runs across both
  trees carried 1-4 of these; suite summaries + STRESS 120/120 still
  complete; `System halt.` present): `SHELLTEST WATCHDOG: protocol stalled
  25 s`, `shell_test3: FAILED ls -l /SUB1`, `[NFS] rpc: no reply from
  server` -> `[NFS] FAILED (1 of 60)`.

## Common Pitfalls

0. **A merged-tip battery must exercise the pristine path.** Lane worktrees
   carry fetched/generated state (extracted vendor trees, `_staging`, built
   archives) that hides bootstrap gaps in the Makefile — the lane gate is
   green while a fresh clone or the CI VM fails instantly.  When a wave
   vendors a new dependency (or adds files the build consumes from
   `third_party/`), the battery must include a from-pristine build: either
   simulate locally by moving the extracted tree aside, or ensure the VM
   tier copies start without the fetched state.  Battery failures that die in
   seconds (missing markers, no run output) are build-bootstrap failures, not
   test failures — read the log before the timer.
1. **On-disk names must fit FAT16 8.3 (≤8 chars + `.BIN`).** mtools accepts longer names by writing VFAT long-file-name entries, but the kernel's FAT16 reader resolves only the plain 8.3 name — the loader prints `Failed to read <NAME>.BIN from disk!` and the program silently never runs. Worse, the suite only halts once every process has drained, so a dropped program hangs the whole run (easily misread as memory starvation). Keep test wrappers to 8 chars with the `_T` suffix convention (`EDITOR_T.BIN`, `TSORT_T.BIN`, `PASTE_T.BIN`), not `<TOOL>TEST.BIN`.
2. **Spawn-test helpers must drain STDOUT before STDERR.** Kernel pipes hold only `PIPE_SIZE` (512) bytes; a child whose output exceeds that blocks mid-write. A helper that reads stderr first (waiting for EOF) deadlocks: the child waits forever for stdout space while the helper waits for stderr EOF that never comes. Read stdout to EOF first (EOF only arrives after the child exits), then stderr. This bit `tac_test.c` on a 10KB output case.
3. **The completion marker is the final `System halt.` line.** MODE=test boots, queues every test, and the boot core prints `System halt.` only after the last process exits and `scheduler_finished()` longjmps back. If the log grows forever with no `System halt.`, a program is still alive — find it; don't wait.
3. **Adding a test that never halts QEMU.** A passing test that doesn't trigger shutdown reads as a 30 s deadlock. Ensure the tier's exit path fires (`-action shutdown=poweroff`; ARM `-semihosting`).
2. **Testing kernel-dependent logic on the host tier** (or vice-versa). Host tests use mock syscalls — they can't validate real scheduling/driver behavior; don't trust them for it.
3. **Ignoring a timeout as "flaky."** A timeout is a first-class failure here. Re-running without investigating wastes cycles and hides a real deadlock.
4. **Wrong ARCH.** RDMA/PCIe/GPU tests are x86_64-only; running them on ARM silently no-ops.
5. **Stale `disk.img` / zombie QEMU.** If results look impossible, `make clean` (kills qemu, wipes artifacts) and re-run. QEMU takes a write lock on `disk.img`, so a leftover ARM/x64 QEMU makes the other arch's run die instantly with `Failed to get "write" lock` — kill the old VM, don't rebuild.
6. **Only one QEMU at a time.** ARM and x86_64 runs share `disk.img` and cannot run concurrently; sequence them (or use `make clean` between).

## Verification Checklist

- [ ] Ran the cheapest tier that can actually catch the change.
- [ ] Saw an explicit `PASS`/`UNIT TESTS PASSED`/summary — not just "no error."
- [ ] Any run >30 s was stopped and investigated as a deadlock, not re-run blindly.
- [ ] New tests halt QEMU on completion.
- [ ] No leftover `qemu-system-*` processes.
