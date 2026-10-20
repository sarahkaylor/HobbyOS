# GB-012-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `0d38100` (clean; `HEAD == origin/main`).
Gate: exactly the four DoD commands — `make unit_tests_arm unit_tests_intel`,
`bash tools/t3_tier.sh arm`, `bash tools/t3_tier.sh intel`.

**DoD item 4 is explicit: any red leg is a RED verdict, including one caused by a known blocker.**
Two legs are red on their own account, and the T2 pair produced a false green.

## 1. `make unit_tests_arm unit_tests_intel` — T2B wedges, and the target reports EXIT=0 anyway

ARM (`T2A`) ran the kernel unit suites to completion with **0 FAIL lines** (no per-suite summary is
printed by this mode, so there is no "0 failures" line to quote — see finding 3).

Intel (`T2B`) never finished. It stopped inside the first suite and then repeated a lock report for
11+ minutes:

```
frame_test_suite:
  Running test_frame_counts...
  ...
  Running test_frame_image_reserved...

[LOCKFOREVER] cpu=0 lock=0x00000000755fe908 pc=0x0000000070050cbe pc2=0x0000000070014bb0

[LOCKFOREVER] cpu=0 lock=0x00000000755fe908 pc=0x0000000070050cbe pc2=0x0000000070014bb0
   (repeated until terminated at 11m12s elapsed; QEMU was killed — a hang is a defect, not a retry)
```

The same test completes on ARM (`[imgrsv] iters=929 high=0x000000023F800000 … wrapped=1`), so this is
not a slow test — it is the x64 lock wedge already open as GB-060 (`LOCKFOREVER`, exactly one claimant).

And then the target's exit status lied about it:

```
make unit_tests EXIT=0
```

`make unit_tests_*` propagates QEMU's exit code, and a QEMU terminated after its guest wedged still
yields 0 — the same "exit status is not a verdict" defect GB-057 was minted for, now in the T2 path.
**T2 cannot fail on a hang.**

## 2. `bash tools/t3_tier.sh arm` — FAIL, no halt

```
=== T3 (arm): build ===
=== T3 (arm): run (deadline 2700s, log …/obj/t3_arm.log) ===
T3_arm: 226504 console lines, 2 FAIL lines, 5 user memory faults, 83 programs exited, halted=0
T3_arm: FAIL — the console has no 'System halt' line (idle/hung wave, or the 2700s deadline hit; a hang is a defect, not a flake)
T3_arm: FAIL — 2 FAIL line(s); first 20:
2281: : FAIL
2719:CXX_T FAILURES
T3_arm EXIT=1
```

The wave finishes its programs and then dumps `[ALIVE-SET]`/`[IDLESTUCK]` forever instead of halting
(`[IDLESTUCK] slot=62 … TORTURE.BIN`, `claims: c0=-1 … c7=-1`) — the GB-063 idle-no-halt shape, on ARM.

## 3. `bash tools/t3_tier.sh intel` — FAIL, 5 FAIL lines + 48 memory faults

```
T3_intel: 880 console lines, 5 FAIL lines, 48 user memory faults, 63 programs exited, halted=1
T3_intel: FAIL — 5 FAIL line(s); first 20:
532:Graphics Validation: FAILED (Pixel mismatch!)
806:  FAIL: line startpoint
807:  FAIL: line midpoint
808:  FAIL: line endpoint
819:GRAPHICS TESTS FAILED
T3_intel EXIT=1
```

The graphics rows are GB-065 (`mmu_map_user_framebuffer` mapping 4 MB of an 8 MB framebuffer; fix
`3a86967` exists only on `ticket/65-line-assert`, never merged); the 48 user memory faults are GB-068's
measured baseline. Both are open blockers — which does not change the verdict (DoD item 4).

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | T2A and T2B both 0 failures | **FAIL** — T2B wedged in `test_frame_image_reserved` (LOCKFOREVER), and the target still exited 0 |
| 2 | `t3_tier.sh arm` 0 FAIL + `System halt` | **FAIL** — 2 FAIL lines, `halted=0` |
| 3 | `t3_tier.sh intel` 0 FAIL + `System halt` | **FAIL** — 5 FAIL lines (GB-065 graphics), 48 user memory faults, `halted=1` but tier red |
| 4 | any red leg ⇒ RED | RED |

## Findings beyond the four legs (for whoever fixes this)

1. **T2 has no output-based verdict.** `make unit_tests_*` is `MODE=unit_tests run` and inherits QEMU's
   exit code; a wedged, killed guest yields 0. T3 got `tools/t3_tier.sh` for exactly this reason — T2
   did not, so "T2 green" is currently unfalsifiable.
2. **No per-suite summary in T2 mode.** The guest prints `Running test_x…` lines only; the DoD asks for
   "0 failures, quoted from the output", which this mode cannot produce. Either print a summary line or
   change the DoD to a FAIL-line count.
3. **ARM T2 completed with 0 FAIL lines** — recorded, but it is not a verdict for the reason above.

## Reproduce

```
export PATH=/usr/sbin:$PATH          # mkfs.fat lives there; without it disk.img errors 127
make unit_tests_arm unit_tests_intel # intel wedges; kill QEMU; note EXIT=0 anyway
bash tools/t3_tier.sh arm            # ~45 min to deadline, then FAIL (no halt)
bash tools/t3_tier.sh intel          # ~6 min, FAIL (graphics)
```

Worktree provisioning, stated because it changes build time but not results: a fresh `hermes -w`
worktree carried neither `third_party/llvm-21.1.8/dist-staging` (227 MB) nor
`third_party/libcxx-21.1.8/src`; I symlinked both from the primary tree (GB-006's gap), which also
avoids a 2613-step cold LLVM build.
