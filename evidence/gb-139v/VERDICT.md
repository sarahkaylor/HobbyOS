# GB-139-V — RED at 770152d (verifier run, 2026-10-30)

Graded head **`770152d`** (PR #264, `fix/verdict-em-dash-shape`), 1 file, +14/−2.
Before-tree for the count: **`a1f4fd4`** (the head #302 gated). Main-tree suites: **`origin/main` = `ebbf4c5b`**.

Clauses 1–5 pass. Clause 6 — the census — fails the card on its own rule: an unhandled
verdict-declaration shape is present on the board with **n=7**.

## Clause 1 — the two house shapes ✓

```
  [OK] '**VERDICT — GREEN (GB-060-V2).** the gate re-ran clean'      -> GREEN
  [OK] '**GB-101-V6 — GREEN at `1dd7160`**'                          -> GREEN
```

## Clause 2 — nothing from the four earlier gates regressed ✓ (8/8)

```
  [OK] 'red or pending run is not a defect and a green run is not correctness'      -> None
  [OK] 'RED fixed (F1 + F2) — pushed to ticket/GB-098-stale / PR #157'              -> None
  [OK] 'red log:   T3_intel: FAIL — 1 FAIL line(s); … rc=1'                          -> None
  [OK] 'The string `VERDICT: GREEN` in prose is documentation, not a verdict'       -> None
  [OK] 'REDs and GREEN-gated helpers are not verdicts'                              -> None
  [OK] '## GB-136-V3 — **RED**'                                                      -> RED
  [OK] '**VERDICT: RED**'                                                            -> RED
  [OK] 'GREEN — GB-114-V2 verified at 899d2a8'                                       -> GREEN
```

## Clause 3 — the count, and the six named

```
BEFORE a1f4fd4 : 68 checked, 9 mismatched, 26 superseded
AFTER  head    : 68 checked, 1 mismatched, 35 superseded
```

The card says before 7; the board moved between minting and this run, so I name the **nine**
that changed on my before-tree rather than inheriting a number:

| card | deciding line on the graded tree |
|---|---|
| GB-044 (#44) | `**VERDICT — GREEN (GB-044-V).** Gate run by the DEEP Verifier on swarm-farm-1 …` |
| GB-060 (#62) | `**VERDICT — GREEN (GB-060-V2).** Gate tree 832342f = origin/main edd60fd × ticket/060-lockcnt …` |
| GB-066-V2 (#74) | `**VERDICT — GREEN (GB-066-V3).** The re-scoped wall has moved …` |
| GB-085 (#136) | `**VERDICT — GREEN (GB-085-V, round 2).** Gate run by the DEEP Verifier …` |
| GB-099-V (#161) | `**VERDICT — GREEN (GB-099-V, round 3).** Gate run … against plain origin/main @ 96ba7d2` |
| GB-106-V (#200) | `**VERDICT — GREEN (GB-106-V2, round 2).** Re-gated on the new head 3da727c …` |
| GB-098 (#159) | `**VERDICT — GREEN (GB-098-V, round 2).** …` — one of #302's three fictions, now correct |
| GB-105 (#197) | `**VERDICT — GREEN (GB-105-V, #199).** …` — the other ficition that used to invert a GREEN |
| GB-101-V6 | folded into the parent: the run now reports `GB-101`, not the gate card |

All six the card names are in the list, each with the line that decided it. No count dropped
without a name.

## Clause 4 — the remaining one is real ✓

```
MISMATCH GB-101 = GREEN but GB-101#179 is in 'Backlog' (want 'Merge Gate')
```

Board state, not parser state: GB-101 carries a GREEN and sits in Backlog.

## Clause 5 — suites, both trees

```
origin/main ebbf4c5b : groom_test 0 · check_stale_test 0 · drift_check_test 0 · check_verdicts --self-test 0 (fails=0)
head      770152d    : groom_test 0 · check_stale_test 0 · drift_check_test 0 · check_verdicts --self-test 0 (fails=0)
```

## Clause 6 — the census, and the shape the eighth gate will miss

Every comment on the board (5 612 comments) reduced to verdict-ish lines:

```
distinct shapes: 3183   (n>=2: 618)
lines containing a GREEN/RED token: 4250   parsed as a verdict: 328 (7.7%)
```

7.7% is not the coverage number — most of those lines are MISMATCH/SUPERSEDED/merge_pr output
and prose that *should* parse as nothing. The honest denominator is **declaration-shaped** lines
(line-initial, starting with `VERDICT`, a `GB-nnn` id, or a heading):

```
declaration-shaped lines: 485 in 306 distinct shapes (n>=2: 108)
parsed: 286 (59.0%)
shapes with n>=2 that parse: 85/108 (78.7%)
```

**Unhandled declaration shapes with n≥2 — the eighth gate's blind spots:**

```
n=7  ## VERDICT on <GB>: **<V>** — back to In Progress (gate ticket <GB> <num>, 2026-…)
     ## VERDICT on GB-070: **RED** — back to In Progress (gate ticket GB-070-V #97, 2026-10-21)

n=6  <GB> <num> closed <V>                       GB-060-V #170  closed RED
n=4  <GB> (<num>) = "VERDICT: <V>"               GB-017-V (#85)  = "VERDICT: RED"
n=3  <GB> (<num>) = "VERDICT: <V>" at <sha> …    GB-014-V (#295) = "VERDICT: RED" at 61e49d1e
n=3  - <GB> <V> superseded by <GB> (<sha>)       - GB-060-V RED superseded by GB-060-V2 (9db1388)
n=3  - **<GB> (<num>) → Merge Gate** (<V>, §4.2 step 5).   - **GB-103 (#189) → Merge Gate** (GREEN, §4.2 step 5).
n=3  - **<GB> (<num>) → In Progress** (<V>, §4.2 step 5).  - **GB-101 (#180) → In Progress** (RED, §4.2 step 5).
```

The first is the one that matters: `## VERDICT on GB-070: **RED** — back to In Progress (gate
ticket GB-070-V #97, 2026-10-21)` is an unambiguous declaration — it opens with `VERDICT`, names
the card, and gives the value — and the parser returns `None` for it. Seven of them exist.

On GB-070 itself this is harmless today, because the card carries four other verdict comments
(`**VERDICT: RED — …**`, `VERDICT: **RED**`, `**VERDICT: GREEN**`) and the newest of those wins:

```
#79 c2508 RED · c3882 NOT PARSED (## VERDICT on GB-070: **RED**) · c8914 RED · c8969 GREEN
```

But on a card where that shape is the *only* verdict, the card reads as having no verdict — and
`check_verdicts` then reports nothing for it, or falls back to an older gate. That is the same
failure mode as the three fictions #302 found, in the other direction, and it is why the card
says an unhandled shape is the finding.

## Verdict

**RED**, on the card's own instruction: *"If a shape is unhandled, that is the finding, and RED is
the right verdict regardless of the four clauses above."* One canonical shape (n=7) and six
lesser ones (n≥3) are unhandled; declaration coverage is 59% of lines and 79% of shapes with n≥2.

The fix itself is good and should not be reverted — the em-dash shape is what made 62 → 35 and
9 → 1, and it is why the six named cards can now be read. What it needs is the `VERDICT on <card>:`
variant and, ideally, a shape census like this one run as a test, so coverage is a number in the
suite rather than a discovery in the next gate.

Minimal addition for `verdict_line_of`: allow `VERDICT\s+(?:on\s+<id>\s*)?[:\-—]` before the
token, i.e. treat `VERDICT on GB-070: **RED**` as the same family as `VERDICT: RED` and
`VERDICT — RED`. That closes the n=7 shape without loosening the prose guard, since the line
still has to start with `VERDICT`.