# GB-093 — SEDTEST intel content-mismatch: classification + named root cause (2026-10-27)

## Gate run (local; no CI)

Fresh worktree (hermes-e046ff70), branch `ticket/93-sedtest-intel` off origin/main. disk.img built (`make disk.img ARCH=intel`, exit 0). Ran `bash tools/t3_tier.sh intel` (T3_LOG=/tmp/t3_intel_gb093.log, T3_SMP=8).

```
T3_intel: 25237 console lines, 141 FAIL lines, 15 user memory fault records across 13 processes, 86 programs exited, halted=1
T3_intel: EXIT=1   (1 FATAL kernel-mode fault line -> not a clean halt; that leg is GB-100's)
```

## SEDTEST classification (tools/board/classify_t3_fails.py /tmp/t3_intel_gb093.log)

SEDTEST total=70, artifacts=0, real=70 — every FAIL is a `(stdout)` content mismatch.

## Root cause, named as a specific expression

Every SEDTEST `got` is the **correct** sed output followed by content the writer never wrote:

```
[SEDTEST] FAIL basic_s (stdout)
  got:  "ALPHA\nbeta\ngamma\ndelta\n\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00..."
  want: "ALPHA\nbeta\ngamma\ndelta\n"
[SEDTEST] FAIL amp (stdout)
  got:  "x<1> y22 z333\n\x00Zmma\ndeltZZ\n\x00\x00\x00\x00..."     # \x00 + NEXT case's fragments
  want: "x<1> y22 z333\n"
```

The specific expression: **`sed_test.c:168`'s read loop keeps reading `out_p[0]` until `r <= 0`, and on intel `pipe_read` returns bytes past the extent the SED writer placed** — the correct prefix is byte-exact (`ALPHA\nbeta\ngamma\ndelta\n`), then a stale ring tail (`\x00` runs; in `amp`, literal fragments of the *following* case `backref` = `Zmma\ndeltZZ`). The writer wrote fewer bytes than the reader consumed; the SED port's stdout itself is correct (exit status is right, prefix byte-exact). This is the pipe ring-accounting class, distinct from TSORTTEST's `-1 != 0` wait-decode artifact (that is GB-095/GB-057). On ARM the same cases pass (0 content mismatches) — the read never overruns. 

## Honest scope

Deliverable-now: **classification + root cause named** (the DoD's explicit non-metric clause, which stays meaningful "while GB-082 and GB-092 are open").
Gated: the DoD's "**0 failures on a run that reaches halted=1**" requires a *clean* intel halt; this wave halted with `halted=1` but carried 1 FATAL kernel fault → the clean-halt leg is GB-100's (abm holder, In Progress). ARM stays 0. Card kept **In Progress** accordingly.

## ARM leg — CLOSED (2026-10-28)

Re-ran the ARM tier on the rebased branch (origin/main merged, incl. GB-113 wait-decode). `tools/t3_tier.sh arm` (T3_SMP=8):

```
T3_arm: 9349 console lines, 0 FAIL lines, 5 user memory fault records across 4 processes, 84 programs exited, halted=1
T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 9349 lines)
[SEDTEST] ALL PASSED (59 checks)
```

**ARM SEDTEST: 59/59 PASS, 0 FAILs** — the DoD's "ARM stays at 0" clause is now evidenced on this tree (raw log: t3_arm_gb093.log.xz). The DoD's remaining leg is the intel clean-halt (still GB-100-gated; this intel wave carried a kernel fault).

## Files
- raw tier log: /tmp/t3_intel_gb093.log (25237 lines) → t3_intel_gb093.log.xz
- ARM tier log: t3_arm_gb093.log.xz

