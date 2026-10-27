**GREEN — GB-115-V2, verified on this VM (DEEP verifier `wgfx-verify`), code PR #225 @ `8d14d88`. Both edge cases are reproduced live on GB-115 itself, and the only differential against `main` is the one the card names.**

Evidence: `evidence/gb-115-v2/` (live replay, boundary fixture, both lint runs with timestamps, self-tests). Trees: `wt-gb115v2` @ `8d14d88`, `wt-gb115v2-main` @ `cb27a8b`. Local only; no CI is cited, used, or gated on.

## 1. Same-second is pending

Self-test line:
```
  PASS: a gate minted in the SAME second as the lane move is pending
```
The rule, `tools/board/groom.py:277`:
```python
    return bool(minted and entry and minted >= entry)
```
Live: GB-115's real lane move is `2026-10-27T17:04:42Z`, and its first gate `GB-115-V` (#232) was created at the same second — the new detector calls it pending:
```
gate #232 (GB-115-V) created_at 2026-10-27T17:04:42Z
  gate_is_pending  OLD -> False
  gate_is_pending  NEW -> True
```

## 2. The older-gate case still flags

Self-test:
```
  PASS: a gate older than the lane move is still flagged
```
And the constructed control (gate created one second before the move):
```
control 2: gate created one second BEFORE the move (17:04:41Z)
  findings        -> [('unverified-in-review', 'GB-900', "in 'In Review' but gate GB-900-V has not posted GREEN")]
```

## 3. A quoted lane line does not move the card — live, not only a fixture

Self-test:
```
  PASS: a lane-move line quoted inside prose does not move the card
```
Live reproduction on **GB-115 (#231)** itself, whose EVIDENCE comment quotes `GB-114: lane In Progress -> **In Review**` inside 38 lines of prose:
```
  2026-10-27T17:04:42Z  lines= 1 len=   36  'GB-115: lane Backlog → **In Review**'
  2026-10-27T17:04:55Z  lines=38 len= 1943  '**EVIDENCE — `GB-115` (satisfies the "in In Review without an EVIDENCE comment" lint line).**'

lane_entry_time  OLD -> 2026-10-27T17:04:55Z      (the quote inside the EVIDENCE comment)
lane_entry_time  NEW -> 2026-10-27T17:04:42Z      (the actual move)
```
That is the whole defect: with `lane_entry_time` = 17:04:55, gate #232 (17:04:42) is "older than the entry" and the old rule flags a correctly-ordered card; with 17:04:42 it is pending.

## 4. Nothing else was cleared

Both tools, same board, 91 seconds apart (`board.py lint` walks the whole board):
```
$ python3 tools/board/board.py lint   # 8d14d88, 2026-10-27T17:12:49Z
LINT clean
$ python3 tools/board/board.py lint   # cb27a8b (main), 2026-10-27T17:12:50Z
LINT clean
```
So no finding disappears, because there are none to disappear. The differential is at the function level and it is exactly the named cases: for GB-115 the two detectors disagree only on the entry timestamp (17:04:55 quoted line vs 17:04:42 move), which is the quoted-line case, and the same card is the same-second case. I found no second disappearing finding.

## 5. Self-tests and lint

```
groom self-test: 26 checks, 0 failed -- ALL PASSED
LINT clean            (live board, 8d14d88)
```

## 6. Boundary justification — and the failing case the card asked for

`>=` is right for the question `gate_is_pending` asks: a gate cannot produce a verdict before its own creation, so a gate minted in the same second as the lane move provably has not run. Strictly-older gates had every opportunity and are still flagged (clause 2).

The exemption is not free, and here is the case it does not cover: **a same-second gate that has already posted RED is masked.** The finding loop consults `gate_is_pending()` whenever the verdict is not GREEN, so a gate minted at T that RED'd seconds later leaves `unverified-in-review` silent:
```
fixture: lane move at 2026-10-27T17:04:42Z / gate created at 2026-10-27T17:04:42Z / gate posted RED at 17:04:47Z
  gate_is_pending -> True
  findings        -> []
```
That is a hole in this lint line, but not an uncovered one: the lane lie it represents is `check_verdicts.py`'s job, and that tool does flag it —
```
  [OK  ] verdict=RED   parent in 'In Review'   -> ok=False (expected False, want-lane 'In Progress')
```
So: not a clause-6 failure (`>=` is not too permissive for "has not run yet"), but if this lint is ever the only reader of a card, that same-second RED is invisible to it. Recording it here rather than asserting there is no case.

One dependency worth naming: the quoted-line rule requires the move to be one line under 160 chars matching `<id>: lane … -> **<lane>**`. `board.py:275` writes exactly `f"{a.gid}: lane {prev or '-'} → **{a.lane}**"`, so every tool-written move qualifies; a hand-written move in another shape would be ignored, which fails toward flagging (the old behaviour), never toward silence.

## Verdict

Clauses 1-6 quoted. GB-115 (#231) → **Merge Gate**; GB-115-V2 (#233) → **Done**.