# L3 attribution evidence — pristine-main wave gate (2026-10-14)

Purpose: prove the only non-LFN FAIL tokens seen on the L3 tree during the
gates are pre-existing/ambient at this tip, not caused by the lane's changes.

Method (per hobbyos-run-tests "Wave FAIL attribution" rule): run the same ARM
wave gate on a PRISTINE tree in the same window and compare failure forms.

- Pristine tree: /home/sarah/hobbyos-selfhost @ ee22ac0 (main, UNMODIFIED —
  no LFN fixtures, no LFN_T, no fat16 changes), copied to
  /home/sarah/.hermes/cache/scratch/l3-pristine (raw log:
  docs/selfhost/lanes/l3-pristine-gate.log there; the scratch copy is pruned
  after 24h, so the relevant lines are recorded below).
- The pristine run ran CONCURRENT with the L3 run3 (same host load) for a fair
  comparison.
- On L3 (run1/run2) the extra FAIL tokens were EXACTLY the two [LFN] test
  failures (fixed since) + the LFN_T summary line; every other token matched
  the pristine tree's token set.

Pristine gate scan (unmodified tree, same window):
  System halt from CPU 3.          <- completion marker present
  FAIL token count: 1
  FAIL lines:
    SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST.
  suite summaries: 18               <- full wave completed

Conclusion: `SHELLTEST WATCHDOG / shell protocol stalls` are ambient at this
tip (present on the untouched tree with zero other failures; documented wave
flake class). The L3 tree's final gate (run5) shows ZERO FAIL tokens.

Also documented in-tree: CXX_T condvar_wait_notify FAIL carries the
"thread_create: no free slot (used=63/63 done=0)" forensic — the documented
launch-slot-pressure flake class (hobbyos-run-tests), not a LFN regression.
run3 additionally hit the documented slot-famine loader wedge under concurrent
build load (forensics: "no free process slots! zombies=1 exited=1",
"thread_create: no free slot (used=63/63 done=0)"); a solo re-run (run5,
quiet box) completed the full wave with zero FAIL tokens.
