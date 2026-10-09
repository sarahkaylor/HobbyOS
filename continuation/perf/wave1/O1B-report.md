# O1b — kernel find-vs-insert hardening (sys_brk / vm_region race)

Lane O1b (subagent). Tree `~/hobbyos-lanes/perf-os`, branch `browser/perf-os`.

## Commits
- `7b8b513` — vm.c/vm.h: `*_locked` inner region ops + self-locking public
  `vm_region_find`/`vm_hole_find`; every in-lock caller converted; mmap
  (fixed + hint), munmap, madvise, map_fb now one-critical-section; new
  `vm_region_cover()`.
- `071bef7` — process.c: `sys_brk` grow path uses `vm_region_cover()`
  (covered-check + reserve atomic under vm_lock) — the run-802 root cause.

## What was hardened
- sudo list in `evidence/AUDIT.md`. Production kernel has ZERO lock-free
  region walks racing a mutation; the public find/hole_find are self-locking;
  mutation-adjacent callers use `*_locked` inside one vm_lock CS.
- Verified: `make ARCH=intel|arm MODE=unit_tests hobbyos.elf` exit 0 at HEAD
  (kernel + vm_test.o linked, zero errors). vm.c/process.c compile clean on
  both arches (no new warnings).

## Evidence (o1/evidence/)
- `brk-race-before.log` — host model, old pattern: **29 spurious -ENOMEM /
  2,500,008 finds across 8 runs**, reproduced every run (run-802 shape).
- `brk-race-after.log` — fixed pattern: **0 spurious / 2,000,006 finds**.
- `AUDIT.md` — full caller audit table + per-site rationale + API contract.
- `probe/brk_race.c` — the model (build: cc -O2 -pthread).

## RUN-REQUEST (o1/RUN-REQUEST.md)
Non-blocking guest legs for the controller (smoke, unit-tests boot, or the
run-802 A/B rerun on the integration branch). P3c's userland tolerance is
untouched — this kernel fix removes the source for every syscall
(find-vs-insert), so it must stand on its own; host A/B + build proofs show
it does.

## Next steps
- Controller: merge `browser/perf-os` `071bef7` into the run tree(s) for the
  next full-stack run (run 802 A/B on both layers); integration will exercise
  brk under real pool-thread spawn.
- Optional: repeat the host probe with more writers for a denser before-count
  (rate is workload-dependent; per-run >0 is the invariant that matters).
