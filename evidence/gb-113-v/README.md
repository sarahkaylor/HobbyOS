# GB-113-V evidence — the intel wait word, and the SIGPIPE harness race

Verifier: `wgfx-verify` (DEEP, swarm-farm-2). Card: GB-113-V (#227), verifies GB-113 (#226),
code under test PR #222 head **`d21759c`** (branch `ticket/113-waitstatus`).

All runs are LOCAL tiers on this VM — no CI is involved, cited, or gated on.

## Trees and commands

| tree | contents | command |
|---|---|---|
| `wt-gb113v` @ `d21759c` | code under test, unmodified | `tools/t3_tier.sh intel` (full wave) → `t3_intel_fix.log.gz` |
| same | focused one-program wave | `bash evidence/gb-113-v/t3_tsort_only.sh evidence/gb-113-v/tsort_fixed_intel.log intel` |
| `wt-gb113v-revert` @ `d21759c` + `control_revert.patch` | **only** the drain/reap change reverted (old: stderr→EOF, `close(err_p[0])`, stdout→EOF, `close(out_p[0])`, `waitpid` last). Raw-word printing on both paths untouched | `bash evidence/gb-113-v/t3_tsort_only.sh evidence/gb-113-v/tsort_revert_intel.log intel` |

`t3_tsort_only.sh` stages `obj/intel/tsort_test.bin` as `::TSORT_T.BIN` plus a one-line
`::WAVE.CFG`, drives the real `tools/t3_tier.sh intel`, and asserts the guest halted, the program
loaded, the wait word was printed, and the kernel did not fault. The tier's completeness-floor FAIL
is expected for a one-program wave and is not read as a result.

## Numbers

| run | TSORTTEST checks | wait words | `0x00000000` | `0x0000000d` | exit-status FAILs | stderr-content FAILs | `FAILURES:` |
|---|---|---|---|---|---|---|---|
| fixed, full wave (41168 lines, halted=1) | 16/wave × 3 | 45 | 45 | 0 | **0** | 4/wave | 4 |
| fixed, focused wave | 16 | 12 | 12 | 0 | **0** | 4 | 4 |
| **control** (drain reverted), focused | 18 lines | 14 | 2 | **12** | **12** | 4 | 14 |

The 4 stderr-content failures are byte-identical in the fixed and control runs (each check emits
both a content FAIL and, in the control, a status FAIL): `cycle from FILE`, `cycle from stdin names
'-'`, `missing file`, `two operands rejected` — `stderr got: ""` in both.

## Files

* `t3_intel_fix.log.gz`, `t3_intel_fix.tier.txt` — full intel wave on `d21759c` (tier itself is red
  for other families: 192 FAIL lines, SEDTEST/CMPTEST/SIGTEST/FPU_T; not this card).
* `tsort_fixed_intel.log`, `.tier.txt` — focused wave, fixed.
* `tsort_revert_intel.log`, `.tier.txt` — focused wave, drain reverted (the control).
* `control_revert.patch` — the exact revert used for the control.
* `analyze_tsort.py`, `analysis_*.txt` — the parsing used for every count above.
* `t3_tsort_only.sh` — the focused-wave driver.