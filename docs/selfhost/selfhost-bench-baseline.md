# Self-host build baseline — `tools/selfhost_bench.sh` A/B protocol (blkcache/selfhost)

Status: **BASELINE V1 — run in progress / completed (see $TIMINGS)**

Purpose: reproducible timing + receipts for HobbyOS's in-OS kernel build
(`MODE=selfhost` boot of the ARM kernel, driver `SELFHOST.BIN` →
`/SRC/SELFHOST.RUN`, serial-relayed markers to `ACCEPTANCE-COMPLETE`),
from a **frozen starting disk state**, so the identical protocol can be
replayed on the cache-enabled kernel (the blkcache A/B arm).

## Protocol (the A/B contract)

1. Freeze a starting disk (see `DISK-STATE.md` in the evidence dir;
   frozen copies live at
   `/home/sarah/hobbyos-lanes/blkcache-evidence/selfhost/` — on disk,
   NOT in git). Two byte-identical copies: `disk-selfhost-base.img`
   (mutated by the baseline run) and `disk-selfhost-abcopy.img`
   (pristine seed for the cache arm).
2. `tools/selfhost_bench.sh <run-disk-copy> <serial-log> --timeout <sec> --tag <name>`
   Boots the disk with the canonical ARM QEMU line (8 cores / 16 GiB /
   MTTCG, `-display none`, serial to the logfile, `-action
   shutdown=poweroff`), records wall-clock (s, from qemu start) at the
   first occurrence of each `[SELFHOST]` marker, and returns:
   - verdict PASS when `[SELFHOST] ACCEPTANCE-COMPLETE` is present
     (markers, never the qemu rc — qemu itself exits 0 on poweroff
     regardless of guest outcome);
   - verdict FAIL on staged failure markers (`FAILED-AT`, `phase=*-fail`,
     `ABORT`);
   - verdict TIMEOUT (with serial tail + qemu state, rc 2) when the
     deadline is hit. Timeout is a parameter: 18000 s for this fresh
     loop (doc budget 2.5–4 h); ~1800–5400 s for an incremental resume.
   Kills ONLY its own qemu (PID trap). Evidence written next to the log:
   `<log>.timing` (marker table), `<log>.verdict`, `<log>.qemu.cmd`.
3. FAIL-token audit on the serial, runner regex (case-sensitive, excludes
   the driver's own contract lines `-fail` / `FAILED-AT`):
   `(?<!-)\bFAIL(?:ED)?\b(?!-AT)` — count by matching lines.
4. Cache arm (later, steered): `cp disk-selfhost-abcopy.img <run-copy>`,
   `tools/selfhost_bench.sh --swap-kernel <run-copy> <cache-kernel.bin>`
   (replaces `/boot/hobbyos.bin`), then the same bench command with the
   same `<serial-log>` naming and the same timeout. The harness refuses
   `*pristine*`/`*abcopy*` paths so frozen copies are never booted or
   mutated.

## Baseline V1 (this document) — the frozen starting state

- Branch/worktree: `blkcache/selfhost` @ `71b980c` (+ commit
  `d78952c` harness).
- Start disk: `disk-selfhost-base.img`
  md5 `bef57f51624bc3ebbeff2863c7dbe316` (= `disk-selfhost-abcopy.img`
  = source `disk.img`, byte-identical, md5-verified).
- State: MODE=selfhost kernel (218,396 B `/boot/hobbyos.bin`), full
  `/TOOLS` in-OS toolchain, `/SRC` = the `--src` build-required source
  subset (235 files), `/SRC.TAR` + `/BUILD_KERNEL.SH` + `/SELFHOST.BIN`.
  **No pre-built `/SRC/obj/arm` objects** (only carried `libc.a`), so the
  in-OS build is a FULL FRESH loop: check → user (HELLO/SH/LS/CAT) →
  kernel (59 TUs, serial) → disk (OUT.IMG) → install.
- Command (run in the background, notified):
  `tools/selfhost_bench.sh …/disk-selfhost-base.img …/baseline-arm-selfhost.serial.log --timeout 18000 --tag baseline-v1`
- QEMU line used: `$LOG.qemu.cmd` (recorded; mirrors the Makefile ARM
  `QEMU_CMD` with `-display none` and `-serial file:$LOG`).

## Result (BASELINE V1 — completed 2026-10-16)

    Run window:        2026-10-16 14:49 CDT → 17:22 CDT (wall)
    DURATION (wall):   9059 s  (= 2 h 30 m 59 s, qemu start → ACCEPTANCE-COMPLETE)
    VERDICT:           PASS — '[SELFHOST] ACCEPTANCE-COMPLETE stages=4' present
                       and 'System halt from CPU 2.'; qemu self-powered-off
                       (`-action shutdown=poweroff`) with rc 0.
    FAIL tokens:       0  (runner regex `(?<!-)\bFAIL(?:ED)?\b(?!-AT)`, matching lines)
    Start disk md5:    bef57f51624bc3ebbeff2863c7dbe316 (the frozen base copy)

### Marker timing table (first occurrence per label, s from qemu start)

    t_s    label                raw line
    ------------------------------------------------------------------
       10  boot-mode-selfhost   Mode: SELFHOST - Running in-OS acceptance build driver...
       10  driver-up            [SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
       10  s0-check-start       [SELFHOST] phase=s0-check-start
       41  s0-check-done        [SELFHOST] phase=s0-check-done rc=0        (check: 31 s)
       41  s1-user-start        [SELFHOST] phase=s1-user-start
     3397  s1-user-done         [SELFHOST] phase=s1-user-done rc=0         (user: 3356 s = 55.9 min)
     3397  s2-kernel-start      [SELFHOST] phase=s2-kernel-start
     8711  s2-kernel-done       [SELFHOST] phase=s2-kernel-done rc=0       (kernel: 5314 s = 88.6 min)
     8711  s3-disk-start        [SELFHOST] phase=s3-disk-start
     9058  s3-disk-done         [SELFHOST] phase=s3-disk-done rc=0         (disk: 347 s = 5.8 min)
     9058  acceptance-complete  [SELFHOST] ACCEPTANCE-COMPLETE stages=4
    ------------------------------------------------------------------
    (no 'System halt.' timestamp: it lands at +1 s in the same final poll)

Stage durations are within (and at the upper edge of) the documented fresh
budgets: user 33–65 min, kernel ~77 min (this run 88.6 min — contention
from the other lanes' VMs), disk ~6 min.  59/59 kernel TUs compiled
(`phase=kernel-compile rc=0` ×59, no failures).

### Decisive serial lines (verbatim)

    Mode: SELFHOST - Running in-OS acceptance build driver...
    [SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
    [SELFHOST] phase=s0-check-start
    [SELFHOST] phase=s0-check-done rc=0
    [SELFHOST] phase=s1-user-start
    [SELFHOST] phase=s1-user-done rc=0
    [SELFHOST] phase=s2-kernel-start
    [SELFHOST] phase=s2-kernel-done rc=0
    [SELFHOST] phase=s3-disk-start
    [SELFHOST] phase=s3-disk-done rc=0
    [SELFHOST] ACCEPTANCE-COMPLETE stages=4
    System halt from CPU 2.

### Artifact receipts (post-run, from the baseline disk copy)

- /SRC/hobbyos.bin (in-OS-built kernel): 218,356 B, sha256
  `00bad4a7…` — **byte-identical** to the w12 acceptance receipt
  (`docs/selfhost/acceptance.md` §10: hobbyos.bin 218356 B sha
  00bad4a7…) → the in-OS build at this tip is byte-reproducible.
- /SRC/OUT.IMG: 134,217,728 B (128 MiB) — in-OS-assembled boot image.
- /SRC/OUT.LOG 26,423 B; /SRC/obj/arm/ populated with the in-OS .o set
  (CHECK/HELLO/SH/SH_AUX/LS/CAT + 59 kernel objects).
- /boot/hobbyos.bin unchanged (218,396 B, sha `7dc47c2e…` = the host
  selfhost-mode kernel): SELFHOST.RUN drives S0–S3 (make check/user/
  kernel/disk); the install-to-/boot and the S4/S5 OUT.IMG boots are
  host-side steps, out of scope for this harness — consistent with
  acceptance.md §5.

## Evidence files (on disk, not committed)

    /home/sarah/hobbyos-lanes/blkcache-evidence/selfhost/
      disk-selfhost-base.img / disk-selfhost-abcopy.img   (frozen pair, md5 bef57f…)
      DISK-STATE.md
      baseline-arm-selfhost.serial.log        (4,135 lines, raw serial)
      baseline-arm-selfhost.serial.log.timing (deduped marker table above)
      baseline-arm-selfhost.serial.log.verdict (verdict=PASS elapsed=9059s …)
      baseline-arm-selfhost.serial.log.qemu.cmd

## A/B replay (cache arm, when steered)

1. `cp disk-selfhost-abcopy.img cache-run.img` (pristine seed, md5 bef57f…)
2. `tools/selfhost_bench.sh --swap-kernel cache-run.img <cache-kernel.bin>`
3. `tools/selfhost_bench.sh cache-run.img cache-arm.serial.log --timeout 18000 --tag cache-v1`

Same frozen disk ⇒ identical read pattern; the only variable is the
kernel (cache-enabled).  The harness refuses `*pristine*`/`*abcopy*`
paths, so the seed can't be booted or mutated by accident.

## x64 datum (no x64 self-host, by design)

The in-OS toolchain is ARM-only and x64 self-host does not exist:
`Makefile.hobbyos` pins `ARCH := aarch64-none-elf` and its kernel TU
list deliberately excludes the x64 arch branch
(docs/selfhost/buildtools-audit.md: "includes the x64 arch branch which
is NOT in the ARM self-host build"); `acceptance.md` §10 lists x64 as
"stretch only (M5) — not required for v1 done / n/a (not attempted)".
No x64 in-OS build was attempted for this baseline (none exists to run).

## Risks / notes

- The fresh-loop baseline is the disk-READ-heaviest selfhost configuration
  (every TU + header read once); the cache A/B arm re-runs the identical
  protocol from the identical frozen disk, so read-cache effects are
  isolated to the kernel swap.
- Known stochastic exec-wedge class: driver self-retries make ×3; a wedged
  attempt shows in the marker stream as a stalled `phase=` with no `-done`.
  The serial tail on timeout captures it; the run is resumable from the
  same (mutated) disk copy via incremental make.
