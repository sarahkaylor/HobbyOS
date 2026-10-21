# GB-075-V — verification evidence (DEEP Verifier, 2026-10-21)

Subject: GB-075 (#103) "the acceptance rig cannot fabricate PASS" — verdict **GREEN for the instrument**,
with the acceptance run itself **FAIL on both arches** (which is the honest result, not a defect in the rig).
Revision: **`badc665`** as pinned by PLAN-008, fresh worktree; `origin/main` has since moved to `79d14e7`,
so this verdict covers `badc665`.

| # | file | what it is |
|---|---|---|
| 00 | `00-arm-build.log` | `make ARCH=arm disk.img` (exit 0), so the harness's QMP window is not racing a cold build |
| 01 | `01-selftest.log` | the ticket's own `tools/t5/acceptance_gate_test.py` → 12 PASS, exit 0 |
| 02 | `02-independent-injections.log` | **my** injection harness (`07-injection-harness.py`), 8/8 behaved as required |
| 03 | `03-arm-run.log` | real `--arch arm` run: per-title observed verdicts, Summary FAIL |
| 04 | `04-intel-build.log` | `make ARCH=intel disk.img` (exit 0) |
| 05 | `05-intel-run.log` | real `--arch intel` run: all five `not in [MENU] dump`, Summary FAIL |
| 06 | `06-arm-mad-matrix.txt` | whole-frame MAD matrix + live flags for the five arm captures |
| 07 | `07-injection-harness.py` | the injection source I ran |

## Clause by clause

**1. A desktop-only frame must FAIL, naming title and reason — proven.** My injection feeds a live,
non-uniform frame with a serial that contains only `DESKTOP.BIN`:
```
OK: desktop-only frame for XNIBBLER fails -> False  reasons=['never loaded (no serial load line / process)']
```

**2. A staged + menu-listed but never-loaded title must FAIL — proven.**
```
OK: XCRYPT staged+listed, never loaded, fails -> False  reasons=['never loaded (no serial load line / process)']
```
The serial in that case contains `mcopy … ::/XCRYPT.BIN` and `[MENU] 20=XCRYPT.BIN` and the frame is live —
exactly the GB-053-V fabrication vector, and it now fails.

**3. Whole-frame liveness — proven at both rows the old sampler missed.**
```
OK: 50x50 block at row 5 reads live -> True
OK: 50x50 block at row 540 reads live -> True
OK: 50x50 block at row 1000 reads live -> True
```
Plus the control that the gate is not simply stuck red:
```
OK: loaded + live frame passes (gate is not stuck red) -> True
OK: near-identical (mad=0.00001) fails margin 0.004 -> False  reasons=['frame changed only 0.000 < margin 0.004 …']
OK: distinct (mad=0.49879) passes margin -> True
INJECTION RESULT: 8/8 behaved as required
```

**4. Real runs, both arches, per-title observed evidence.**
ARM (`03-arm-run.log`):
```
| PONG.BIN     | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | no  | True | FAIL |  reasons: never loaded (no serial load line / process)
| XNIBBLER.BIN | 19 | yes | True | FAIL |  reasons: frame changed only 0.002 < margin 0.004 (not a distinct rendered title)
| XCRYPT.BIN   | 20 | no  | True | FAIL |  reasons: never loaded (no serial load line / process)
| NES.BIN      | 21 | no  | True | FAIL |  reasons: never loaded (no serial load line / process)
Summary: **FAIL**
```
and the serial says exactly that: `PONG.BIN` and `XNIBBLER.BIN` have `Loading program for scheduler (v2 AS):`
lines; MILLIPED/XCRYPT/NES have none. Compare GB-053-V at `0d38100`, where the same wave produced five
PASS rows — this is the fabricated PASS, gone.

INTEL (`05-intel-run.log`): all five `slot -`, `loaded no`, `live False`, `not in [MENU] dump`; the serial
shows why —
```
[KERNEL] pid=1 (DESKTOP.BIN) memory fault VA=0x00000000755ADBC8 … region=NONE … -> killed
[KERNEL] Process 1: DESKTOP.BIN exited unexpectedly or gracefully.
```
No `[MENU]` lines at all (count 0). A missing arch reported as five honest FAILs, not as a skip.

**5. Load-then-hang is RED, not a retry.** XNIBBLER loaded and then did not advance (frame MAD 0.002
against the previous capture, below the stated 0.004 margin) — the rig fails it on the margin rather than
retrying, which is the 30 s rule applied to the one title that did load and sit still.

## The ARM MAD matrix (whole frame, /255)

```
               MILLIPED   NES.ppm  PONG.ppm  XCRYPT.p  XNIBBLER
MILLIPED.ppm     0.0000    0.0095    0.0006    0.0020    0.0017
NES.ppm          0.0095    0.0000    0.0091    0.0097    0.0095
PONG.ppm         0.0006    0.0091    0.0000    0.0017    0.0014
XCRYPT.ppm       0.0020    0.0097    0.0017    0.0000    0.0007
XNIBBLER.ppm     0.0017    0.0095    0.0014    0.0007    0.0000
```
Four of the five captures sit within 0.002 of each other — the GB-053-V signature (0.15–0.97/255) — and
the rig now refuses them. NES's 0.0095 is the screen that was up at the time, not the game: it never
loaded, and the row says so.

## What this verdict does and does not say

It says the **instrument** no longer fabricates PASS: injections fail, a real run fails on real absence,
and every row cites an observed event. It does **not** say S0/S1 is accepted — the same run is FAIL on
both arches, and GB-053 must stay red until the titles actually launch and animate. GB-075 fixed the
measurement; it did not fix the games, and the evidence above should be read that way.
