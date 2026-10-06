# X2 — x64 TSC calibration + LOSTWAKE->#PF at -smp 8: root cause & fix (RESOLVED)

Lane `browser/fs-x2` @ `2f48639` (fix) — filed against browser.md §11.1.

## Symptom (both entries of the mission)

1. **TSC/timer defect:** the x64 guest clock ran ~5-18x fast (browser.md §11.1:
   "TSC rate 35,726 ticks/ms makes the guest clock ~18x fast, collapsing
   LOSTWAKE_DEAD_OWNER_MS=2000 to ~110 real-ms" — on this workstation the same
   defect measured `386,430`-`532,489` ticks/ms vs the host's `2,595,081`, i.e.
   5-13x fast, varying per boot).
2. **-smp 8 desktop crash:** `[LOSTWAKE] disposing pid=1 DESKTOP.BIN (stale
   dead-owner claim, poisoned resume)` => desktop killed / System halt / #PF /
   process_create hang after ~35-46 s of idle (worked around with -smp 1).

## Root cause — TWO independent defects, one amplifier chain

### Defect A: the calibration timebase counted firmware APIC ticks (the amplifier)
`timer_get_ms()` derives from `rdtsc() / tsc_per_ms`, where `tsc_per_ms` was
measured in `lapic_timer_calibration_tick()` as `tsc_used / ((timer_ticks -
calib_start_ticks) * 10)` — 20 `timer_reload()` calls assumed to be 200 ms of
PIT time.

**The firmware (OVMF/EDK2) arms a periodic LAPIC timer on vector 32 and leaves
it unmasked** (`LVTT=0x20020`, div-by-1, initial_count=10,000,000, verified live
via HMP `info lapic` on QEMU 10.2.1). It keeps firing vector-32 as the kernel
boots; the kernel never touches the LVT (its `lapic_timer_start_periodic()` is
`(void)`-discarded on x64). Result: vector-32 arrived at ~500-1400 Hz instead
of the PIT's 100 Hz during early boot (varies per boot with firmware timing),
so "20 ticks" spanned ~30-90 real-ms, `elapsed_ms=200` was 3-7x too large,
`tsc_per_ms` came out 3-7x too small, and `timer_get_ms()` ran that many times
fast. With a 5-13x fast clock, `LOSTWAKE_DEAD_OWNER_MS=2000` collapsed to a few
hundred real-ms; the idle reaper (process.c start_scheduler) saw the desktop's
kernel-mode syscall (IF=0 => heartbeat not refreshed between IRQs) as a stale
dead-owner claim and DISPOSED the live DESKTOP.BIN — the -smp 8 crash. (The
extra interrupts also always corrupted every other early-boot `timer_ticks*10`
read.)

### Defect B: the 8254 channel-0 counter decrements by TWO per clock in mode 3
Replacing the timebase with the PIT channel-0 counter surfaced a second off-by-
two: the counter read in mode 3 is `count - (2*d) % count` (QEMU `i8254.c`
mode 3; the real 8254 also counts by two in square-wave mode). Treating it as
one count per 1.193182 MHz clock made the elapsed window twice its true length
=> `tsc_per_ms` half of true => clock 2.0x fast (measured deterministically:
two independent windows both read 1,297,578 vs the host's 2,595,081).

## Fix (commit `2f48639`, src/kernel/arch/x64/timer.c only)

- **Disarm the firmware APIC timer at `timer_init`:** `LAPIC_TIC_INIT=0` +
  mask the LVT (`32|(1<<16)`), so the vector-32 stream is clean 100 Hz from the
  first PIT tick.
- **Calibrate `tsc_per_ms` against the PIT channel-0 counter**, accumulated in
  a tight IRQ-off loop over ~60 ms (12 periods x 5 ms), immune to the extra
  early interrupts. Wrap-aware accumulation (`c > last` => `last+11931-c`).
- **Mode-3 count-by-two factor:** `elapsed_us = counts * 1000000 / (2*1193182)`.
- **Conservative floor:** a derived rate below 700,000 ticks/ms is treated as a
  broken timebase and the driver falls back to the tick clock (slow > fast for
  lost-wake deadlines). Bias is now "never faster than wall".
- No process.c / LOSTWAKE logic changed: the arm-and-confirm gate (L9) already
  guards the dispose; with a wall-true clock its 2000 ms + ~1.4 s confirm window
  is sane again on x64, matching ARM.

## Verification (this workstation, QEMU 10.2.1, -enable-kvm)

- Calibration line now prints `TSC rate: 2,596,268-2,596,677 ticks/ms`
  (host ground truth `2,595,081`; was `216,985`-`532,489`). Two independent
  in-kernel windows agree to 0.1%.
- Guest clock vs wall via wall-stamped serial: `13315-7916 guest-ms` over
  `7.979 s` => **1.000x** (was 5-13x fast).
- **-smp 8 desktop receipts (post-fix):**
  * run A: 12.5 min idle, 0 `[LOSTWAKE]`, 0 FATAL/#PF/System halt.
  * run B (with QMP): 2:30+ min, 0 faults; QMP key injection accepted and
    tolerated (no app-launch reaction — the x64 start-menu needs a mouse click
    the current tip cannot deliver; X1's in-flight fix).
  * baseline control at same tip/boot: crash at ~35 s (`[LOSTWAKE] disposing
    pid=1` -> desktop gone; log `x2-smp8-baseline1.log`).
- A `[WATCHDOG] console silent for more than 20s` + `CSTK` dump every ~20 s of
  idle console silence is pre-existing and benign (diagnostic on silence; ages
  stay tiny, pid-claim stays live). Not touched.

## Residual / notes

- The arm-and-confirm reaper can still arm on a >2 s continuous IF=0 kernel-mode
  syscall and prints an IDLESTUCK dump (rate-limited); it only disposes when the
  same frozen heartbeat confirms one idle round later — a genuinely wedged core.
  ARM parity. The fast-clock era made this fire every few hundred real-ms; now
  it requires a real 3.4 s+ freeze.
- The x64 guest TSC in this QEMU/KVM config reads the host TSC (2.595 G); the
  pre-fix "386-532k" readings were purely the calibration-window corruption.
- LAPIC-on-device (real hardware): LAPIC timer was left unused on x64 (as
  before); no behavior change.

## Files
- `src/kernel/arch/x64/timer.c` (the only changed source)
- Evidence: `x2-smp8-baseline1.log`, `x2-fix4.log`, `x2-receipt1.log`,
  `x2-hosttests.log` (all in the lane worktree).
