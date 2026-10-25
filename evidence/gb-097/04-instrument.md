**GB-097 — the missing instrument now exists, and the baseline is 113 shredded records in one run.**

The DoD asks for "zero byte-interleaved lines", but nothing in the tree measures *byte* interleaving:
`t4_glued.sh` counts record fusions (two tags on a line), while this defect leaves no second tag — it
alternates characters inside one record. So I built the measurement first, with a self-test that proves
it can fail.

### The detector

`evidence/gb-097/console_shred.py` — a line is shredded when a phrase its writer emits contiguously
(`Exception in Kernel Mode`, `Error Code:`, `freeing root=`, …) survives only as a **subsequence**:

```
$ python3 evidence/gb-097/console_shred.py --self-test
  PASS: fires on specimen (phrase 'Exception in Kernel Mode')
  PASS: clean contiguous record not flagged
  PASS: unrelated record not flagged
self-test: OK                                                        exit=0
```

Build/QEMU command echoes are skipped by prefix, which is what removed the first version's false
positives (long link lines contain the letters of `console silent` in order).

### The baseline, on a real intel run (`9f89a23`)

```
$ python3 evidence/gb-097/console_shred.py evidence/gb-082v/01b-intel-serial.log
console_shred: 113 byte-interleaved record(s) in '…/01b-intel-serial.log'      exit=1
  L487  d/es! rcVe/cketorr: n14el Er/rporr oCogradme_: l0oxa0de00r0.c0
  L564  v2 loader:[ PIKEDRN=E2L2]  eFntArTALy:=0 xE1x0ce00pt0i0o0n 00in0  Kseprne=0l xM18o0d0e0!0…
  L959  [KERNEL] FATAL: Exception in Kve2r nelloa Mdoerd:e!  Vimeagctoer : m14 aEprrpoerd C od(el:…
  L1136 [[SKEEDRTNEESLT]]  stFaArtTAinLg::  E5x9c egpotlidoenn  icna seKse rfnerlo mM oGdNeU!  seVde c4t.o8r
```

113, not 1 — the specimen I posted earlier was one instance of a continuous background phenomenon, not
an isolated hiccup. (Count is a lower bound: it only sees records whose phrases I listed.)

### What this changes for the DoD

| clause | before this comment | now |
|---|---|---|
| "zero byte-interleaved lines" measurable at all | no instrument | **yes** — `console_shred.py`, self-tested, exits 1 on any hit |
| the count on the current tree | unknown | **113** (intel, `9f89a23`, one run) |
| a run that fires `[FREERACE]`/`[STALLBREAK]` | not yet produced | still not produced — my run has 0 such records |

So the ticket is now *gradeable* where it was not: the fix has a number to drive to zero and a gate that
can prove it failed. The precondition run (one that fires `[FREERACE]`/`[STALLBREAK]`) is still missing,
and the fix itself is still the design call I flagged — console ownership versus a build-time prohibition
on raw-sink calls from returning functions.

Status **In Progress**: instrument delivered and self-tested; DoD not met (113 ≠ 0, and no close-firing
run). If the Coordinator wants the detector in-tree rather than in `evidence/`, say so and I will land
`tools/gate/console_shred.py` with the same self-test as a gate target.
