# perf-baseline-REPORT.md — GB-045 performance pass: meaningful gfxbench numbers + boot-time

**wgames, 2026-10-20.** Perf ticket **GB-045** (sarah/gaming-program#45). This report
addresses the GB-045-V RED ("meaningless numbers") and delivers the DoD `numbers + report`.

## Result: the GB-045-V RED is resolved

The verifier's RED was a **measurement defect, not a graphics one**:
`GFXBENCH full min=0 med=1000 mean=800` was impossible for a real full-screen flush.
Root cause (found by inspection of the shipped code, verified on a live ARM boot):

1. **1 ms clock vs sub-ms flush.** Userland `clock_gettime(CLOCK_MONOTONIC)` is backed by
   the kernel `sysinfo(1)` integer-**millisecond** uptime (`src/user/libc.c:885`,
   `tp->tv_nsec = (ms%1000)*1e6`). gfxbench timed a *single* sub-ms `flush_fb()` against
   that clock, so each sample read **0 us or 1000 us** at the mercy of the tick boundary —
   exactly the `min=0 med=1000` signature.
2. **Numbers went to the window, not serial.** gfxbench printed the values with
   `print_dec()` (writes to process stdout). As a desktop-window app that pipe is captured
   by the window, so only the `print_console` labels reached serial; the numbers vanished.

**Fix (this branch, PR sarahkaylor/HobbyOS#66):** burst timing + serial output —
see below. Re-measured on a live ARM boot via the shared evidence rig
(`tools/evidence_rig.py --bin GFXBENCH.BIN --arch arm`, commit 8843e667):

```
GFXBENCH full min=343 med=406 mean=437 n=10
GFXBENCH r256 min=156 med=187 mean=196 n=30
GFXBENCH r64  min=117 med=164 mean=162 n=50
GFXBENCH DONE
```

Meaningful: no 0 us, no exact-1 ms, ordering full > r256 > r64 as the arena size shrinks.

## Numbers (ARM, qemu-system-aarch64, virgl soft, 8 TCG cores)

| phase   | min (us) | med (us) | mean (us) | n   | present-bound FPS budget (1000/mean) |
|---------|----------|----------|-----------|-----|---------------------------------------|
| full    | 343      | 406      | 437       | 10  | ~2,289 Hz                             |
| r256    | 156      | 187      | 196       | 30  | ~5,102 Hz                             |
| r64     | 117      | 164      | 162       | 50  | ~6,173 Hz                             |

Method: per sample, a BURST of flush calls is timed end-to-end (`t1-t0 >> 1 ms`, so the
ms clock resolves it) and divided by the burst count for per-flush us:
`FULL=32, R256=64, R64=128`. min/med/mean over the burst-averaged samples.

Takeaway for the framerate budget: the GPU present (flush) path costs well under 0.5 ms
per full-screen present on this soft-VRAM setup — far inside a 60/144 Hz frame budget and
not the rendering bottleneck. Compare within the same platform+tree only (browser.md §11:
x64 guest clock has a separate ~18x calibration defect).

## Boot-time (ARM, direct QEMU, prebuilt disk.img)

From QEMU spawn to the fully-populated desktop menu (`[MENU] count=128`): **6.41 s**
(`Desktop starting` at the same mark; measured with a timed headless boot, no make
rebuild in the timed path).

## Verification

- T1 host golden: `make gfx_stats_test_host && ./gfx_stats_test_host` -> **14 checks,
  0 failures** (locks min/med/mean, burst division, and the serial u64 formatter).
- Both-arch guest builds: `make obj/arm/gfxbench.bin ARCH=arm` /
  `make obj/intel/gfxbench.bin ARCH=intel` link clean; `_start` at 0x1000000000,
  `gfx_stats_of`/`gfx_per_flush`/`gfx_u64_to_str` present.
- cstyle clean; `make -n host_tests` parses (gfx_stats_test wired into host_tests).

## What changed

- `src/user/gfxbench.c` — burst sampling (FULL 32 / R256 64 / R64 128); report() emits
  min/med/mean/n via `print_console` (serial) so it is capturable in any launch mode.
- `src/user/gfx_stats.{h,c}` (new) — pure, host-testable `gfx_stats_of` (min/med/mean),
  `gfx_per_flush` (burst division), `gfx_u64_to_str` (serial-safe decimal formatting).
- `src/host/gfx_stats_test.c` (new) — T1 golden.
- `Makefile` — link gfx_stats.o into GFXBENCH_BIN (arm+intel); add `gfx_stats_test_host`
  to `host_tests`.

## Evidence artifacts (gitignored `evidence/`)

`evidence/gb045gfx2/20261020T233422Z/`: serial.log (GFXBENCH lines), boot/launch screendumps
(boot.ppm/launch.ppm + .png), replay.jsonl, manifest.json (sha256 of every artifact).
