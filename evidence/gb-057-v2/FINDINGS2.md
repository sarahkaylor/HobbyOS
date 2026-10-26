# GB-057 — eighth pass: DoD (a) IS MET — 3 of 3 intel runs halt at `c71cb87`; the repeat-resume livelock is measured

Builder: `wverify-deep`, DEEP verifier session, 2026-10-26 05:35–05:50Z.
Revision: **`origin/main` `c71cb87`** (includes the merged `DEADCLAIM` fix #184).
**Three new intel T3 runs**, instrumented with the per-`(cpu,pid)` resume counter the handoff named
(patch committed as `instrument-rescnt.patch`; measurement only, prints at 64/256/1024/4096 consecutive
resumes and changes no behaviour).

## DoD (a) — 3 consecutive halts: **MET**

```
RUN 1  05:35:12Z -> 05:38:03Z   T3_intel: 30599 console lines, 162 FAIL lines, 22 user memory fault records across 18 processes, 85 programs exited, halted=1
RUN 2  05:38:03Z -> 05:38:47Z   T3_intel:  9423 console lines,  43 FAIL lines,  5 user memory fault records across  5 processes, 81 programs exited, halted=1
RUN 3  05:38:47Z -> ~05:40Z     T3_intel: 10594 console lines,  94 FAIL lines,  7 user memory fault records across  7 processes, 81 programs exited, halted=1
```

**Stall frequency on this revision: 0 of 3** (against **1 of 3** measured on `41ec708` in the previous
pass, and "2 of the first 3" in the card's own earlier note). All three runs reached `System halt`; none
hit the 900 s deadline. This is the load-bearing clause of the re-derived DoD and it is now satisfied on
`main` as it stands.

The tier is still **red for FAIL lines** in every run (162 / 43 / 94) — that is clause (c)'s subject, not
(a)'s, and I am not conflating them.

## DoD (b) — real vs artifact, per run

```
run1  FAIL lines: 162   artifacts 16 / real 146    tags: SEDTEST 71, TSORTTEST 35, CMPTEST 16, TACTEST 7, SIGTEST 5, TEST 1
run2  FAIL lines:  43   artifacts  7 / real  36    tags: SEDTEST 11, TSORTTEST 10, CMPTEST 6, SIGTEST 1, TACTEST 1
run3  FAIL lines:  94   artifacts  3 / real  91    tags: SEDTEST 59, TSORTTEST 10, COMMTEST 5, CMPTEST 5, TACTEST 3, SIGTEST 1, TEST 1
```
No run carries the EMFILE/fixture-failure inflation that made run 2's count unreadable on `41ec708`
(`Too many open files` = 0 in all three). The spread across runs is load/ordering, not a different corpus:
the same five clusters appear in every run.

## The named instrument, answered: a real repeat-resume livelock, and it does not now block the halt

`[RESCNT]` fires when one CPU resumes the same pid consecutively; worst observed:

```
[RESCNT] cpu=7 pid=2  consecutive_resumes=64
[RESCNT] cpu=7 pid=2  consecutive_resumes=256
[RESCNT] cpu=7 pid=2  consecutive_resumes=1024   sp=0x00000010800C2F80
[RESCNT] cpu=7 pid=46 consecutive_resumes=256    sp=0x00000010800C2F80
[RESCNT] cpu=6 pid=37 consecutive_resumes=64
[RESCNT] cpu=3 pid=37 consecutive_resumes=64
[RESCNT] cpu=3 pid=15 consecutive_resumes=64
[RESCNT] cpu=2 pid=37 consecutive_resumes=64
```

So the shape the handoff called "counters climbing for two of the three claimants — a shape nobody has
explained" is now named: **one CPU re-resuming the same process from the same stack pointer over a
thousand times in a row.** What the measurement also shows is that on `c71cb87` this livelock no longer
prevents the wave from finishing: all three runs halted anyway. It is therefore a *separate* defect from
the halt, and the right card for it is the resume path (GB-100 #178), not this one.

## Clause (c) — still unmet, and now precisely scoped

Carded: `SEDTEST` → **GB-093** (#147), `TSORTTEST` → **GB-095** (#152).
**Uncarded, measured in all three runs:** `CMPTEST` (16 / 6 / 5 real), `TACTEST` (7 / 1 / 3),
`SIGTEST` (5 / 1 / 1). Proposed one-line cards (the Coordinator mints; I am not minting tickets):

* **intel CMPTEST** — cmp assertions fail on intel only; ARM's equivalent is GB-081 (#127) and covers a
  different arch/root cause, so it does not cover this.
* **intel SIGTEST** — `SIGTEST FAIL waitpid returns the child got=14 want=4294964048` is the visible
  instance (a waitpid return of 14 where -4 was expected): a signal/wait delivery mismatch, not a console
  artifact.
* **intel TACTEST** — small cluster (1–7), needs its own card or folding into GB-093 if the root cause is
  the shared `sed`/`cat` fixture path.

Until those three exist, clause (c) ("every real cluster fixed or carded") is **not met** and I am not
claiming the card is done.

## Verdict

This is a debug card: no RED/GREEN verdict. Status: **(a) met — 3 of 3 halts; (b) delivered; (c) unmet —
three named clusters need cards.** Card moves to **In Review** for the Coordinator to mint those three
cards (or fold them), after which GB-057 has no remaining measurement of its own.

## Reproduce

```
git worktree add /tmp/gb057e c71cb87 && cd /tmp/gb057e && mkdir -p obj
git apply <path>/instrument-rescnt.patch      # optional: the resume counter
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel MODE=test hobbyos.elf disk.img
for i in 1 2 3; do T3_LOG=$PWD/evidence/gb-057-v/1$i-t3-intel.log T3_TIMEOUT=900 W0_GATE=0 bash tools/t3_tier.sh intel; done
python3 tools/board/classify_t3_fails.py evidence/gb-057-v/1<i>-t3-intel.log
grep -h RESCNT evidence/gb-057-v/1*-t3-intel.log
```