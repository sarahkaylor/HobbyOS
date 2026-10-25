# GB-057 — DEEP debug pass, 2026-10-25 13:0x–13:4xZ (debug reserve)

Worker: `wverify-deep` (DEEP verifier session, swarm-farm-3). Revision gated: **`origin/main` `41ec708`**
("board lint: re-verified gate cards read correctly (#179)"). Not a RED/GREEN verdict — this card is
debug work and its DoD is measurement. All logs under `evidence/gb-057-v/`.

## DoD (re-derived 2026-10-24) — clause by clause

| Clause | Result |
|---|---|
| (a) `halted=1` on **3 consecutive** intel runs, stall frequency quoted | **NOT MET** — 2 of 3 halted; run 2 stalled to the 900 s deadline |
| (b) real-FAIL count quoted per run, separately from the artifact count, via the classifier | **DELIVERED** — 75/83 real on the halting runs, 256 on the stalled run (with the caveat below) |
| (c) every real cluster fixed or carded, `SEDTEST`/`TSORTTEST` named | **PARTLY** — SEDTEST → GB-093 ✓ and TSORTTEST → GB-095 ✓ are carded; intel CMPTEST, SIGTEST, TACTEST are not |
| (implied by the card's premise) the tier is a gate again | **CONFIRMED**, with a two-sided control |

## Three consecutive intel T3 runs at `41ec708` (`01-three-runs.log`)

```
########## INTEL T3 RUN 1 ##########   13:03:47Z -> 13:04:33Z  (46 s)
T3_intel: 10687 console lines, 84 FAIL lines, 5 user memory fault records across 5 processes, 80 programs exited, halted=1
T3_intel EXIT=1

########## INTEL T3 RUN 2 ##########   13:04:33Z -> 13:20:06Z  (15 m 33 s = the 900 s deadline)
T3_intel: 18246 console lines, 280 FAIL lines, 9 user memory fault records across 9 processes, 86 programs exited, halted=0
T3_intel: FAIL — the console has no 'System halt' line (idle/hung wave, or the 900s deadline hit; a hang is a defect, not a flake)
T3_intel EXIT=1

########## INTEL T3 RUN 3 ##########   13:20:06Z -> 13:21:13Z  (67 s)
T3_intel: 16085 console lines, 93 FAIL lines, 11 user memory fault records across 11 processes, 83 programs exited, halted=1
T3_intel EXIT=1
```

**Stall frequency at this revision: 1 of 3.** Clause (a) is therefore unmet and this card cannot be
closed on this evidence. The tier itself behaved correctly on all three: it returned non-zero and said
why each time, including naming the hang as a hang.

## Clause (b) — classifier per run (`02-classify.log`)

```
run1  FAIL lines: 84   ARTIFACT_FOREIGN 8, ARTIFACT_FRAGMENT 1        -> artifacts 9 / real 75
run2  FAIL lines: 280  ARTIFACT_FOREIGN 18, ARTIFACT_FRAGMENT 6       -> artifacts 24 / real 256
run3  FAIL lines: 93   ARTIFACT_FOREIGN 6, ARTIFACT_FRAGMENT 4        -> artifacts 10 / real 83
```

**The run-2 number must not be read as a test-quality count.** Its "real" lines are dominated by
infrastructure failures that do not exist in either halting run (`08-emfile.log`):

```
run1 (halted=1): 'Too many open files' = 0   'pipe failed' = 0   'cannot write fixtures' = 0
run2 (halted=0): 'Too many open files' = 15  'pipe failed' = 255 'cannot write fixtures' = 49
run3 (halted=1): 'Too many open files' = 0   'pipe failed' = 0   'cannot write fixtures' = 0
```

and 39 `spawn *.BIN failed` lines in run 2 against 0 in both halting runs. The same programs pass in
the halting runs — `[GREPTEST]` 59 PASS / 0 FAIL (run 1) vs 71 PASS / **23 FAIL** (run 2),
`[COMMTEST]` 16/0 vs 23/11, `[NLTEST]` 19/0 vs 31/8, `[MD5TEST]` 15/0 vs 25/8 (`04-tag-census.log`).
So run 2's extra 173 "real" failures measure a degraded machine, not the tests.

## Clause (c) — real clusters and their cards

| Tag | run1 real | run2 real | run3 real | card |
|---|---|---|---|---|
| SEDTEST | 55 | 78 | 54 | **GB-093** (#147, Ready) |
| TSORTTEST | 8 | 16 | 11 | **GB-095** (#152, Ready) |
| CMPTEST | 2 | 23 | 5 | GB-081 (#127) covers **ARM** CMPTEST; intel CMPTEST is not separately carded |
| TACTEST | 2 | 14 | 2 | no card |
| SIGTEST | 0 | 7 | 2 | no card |
| GREP/COMM/NL/UNEXP/MD5/CUT/FOLD/PASTE/EXPAND/TR/WC/HED/TAIL | 0 | 23/11/8/11/8/8/10/7/6/5/4/2/1 | 0 | **not test defects** — EMFILE-driven (see above), so they should not be carded |

## The lead this pass produced — the stall coincides with file-descriptor exhaustion

Ordering inside run 2 (`05-run2-ordering.log`, `06-infra-failures.log`):

```
line   2985  /CAPA.TXT: Too many open files                       <- first EMFILE
line   3396  SED.BIN: can't read SEDT.IN: Too many open files
line   3464  [SEDTEST] FAIL ... (cannot write fixtures)            <- first fixture failure
line   3946  [SEDTEST] FAIL empty_reuse (spawn SED.BIN failed)     <- first spawn failure
line  16795  [IDLESTUCK] ...                                       <- first stall marker (92% through the log)
line  18246  end of log (deadline kill)
```

Every occurrence of `EMFILE` / `pipe failed` / `cannot write fixtures` / `spawn … failed` is **absent**
from both halting runs. The degradation starts ~13,800 lines before the watchdog's own stall dump, so
the stall is not the first symptom — the machine was already failing to open files and spawn processes
long before it stopped making progress.

**This is an association, not a mechanism** — it is one stalled run out of three, and I am not
claiming it is the cause. What it does is give the next pass a cheap, discriminating experiment: run
intel T3 with `MAX_OPEN_FDS` raised (or with the fd table instrumented) and see whether the stall
frequency drops; if it does, the "resume frame / IF bit / #UD at 7" line of enquiry that this card has
chased for six passes is not where the stall lives.

## The tier is a gate — two-sided control (`03-police-control.log`)

Clause (b) of the original card was "222 FAIL lines yet exits 0". That is gone, and this is the proof
that the detector can both fire and stay silent, run in this session on this VM:

```
$ tools/t3_tier.sh police /tmp/t3_nofail.log intel        # run-1 console with the FAIL lines removed
T3_intel (police /tmp/t3_nofail.log): 10604 lines, 0 FAIL lines, 5 fault records across 5 processes, halted=1, kfault=0, boots=1
T3_intel: PASS (0 FAIL lines, halted)
T3_intel EXIT=0

$ tools/t3_tier.sh police /tmp/t3_injected.log intel      # the SAME log + one injected FAIL line
T3_intel (police /tmp/t3_injected.log): 11900 lines, 85 FAIL lines, 5 fault records across 5 processes, halted=1, kfault=0, boots=1
T3_intel: FAIL — 85 FAIL line(s); first 5:
...
T3_intel EXIT=1
```

One synthetic line moved the tier from PASS/0 to FAIL/1. Combined with the three real runs above
(all EXIT=1 with their reasons quoted), the exit-code-ignores-console defect this card was minted for
is fixed and the fix is proven to fail when it should.

## What is still open

1. **The stall (clause a)** — 1 of 3 at `41ec708`; the card's own bar is 3 consecutive, and it is not
   met. Nothing in this pass closes it.
2. **Uncarded real clusters** — intel CMPTEST, SIGTEST, TACTEST have no card. Either card them or fold
   them into an existing one; clause (c) is not met until one of those is true.
3. **The EMFILE lead** — a discriminating experiment is named above; it is a measurement, not a fix.

## Reproduce

```
git worktree add /tmp/gb057 41ec708 && cd /tmp/gb057 && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel MODE=test hobbyos.elf disk.img
for i in 1 2 3; do T3_LOG=$PWD/evidence/gb-057-v/0$i-t3-intel.log T3_TIMEOUT=900 bash tools/t3_tier.sh intel; done
python3 tools/board/classify_t3_fails.py evidence/gb-057-v/0<i>-t3-intel.log
grep -c "Too many open files" evidence/gb-057-v/0<i>-t3-intel.log
```
