# GB-110-V4 (#221) — VERDICT: **RED** (the ordinal selector never runs on real titles)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 05:5xZ.
Verifies GB-110 (#211) / ship **PR #214 @ `1b2fc86`**. Host-side T1 only; no CI consulted. I did not author
GB-110 or PR #214.

**Tree: `1b2fc862a37a17c131812c7a914d91291e869f63`.**

## One line

`--pr 204` does ALLOW and name `3da727c…` — the card's headline outcome — but **not through the ordinal
selection this PR adds**. `gate_orders` is built with `gb_v_re.match(t)` against the **whole title**, and
every real verify title is `"GB-110-V4 Verify …"`, which that `^GB-(\d{3})-V(\d*)$` regex rejects. So the
candidate list is always `[owner-card]`: the ordinal sort is dead code on this board, and the ALLOW came
from the owner card's own newest GREEN. Measured, not argued, below.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | `--pr 204` ALLOWs naming `3da727c…`; before/after quoted | **PASS (outcome)** — but by the owner card, not the selector |
| 2 | `--pr 213` still ALLOWs on `d245ead…` | **PASS** |
| 3 | ordinal ordering is **structural**: candidate list ordered by gate ordinal, V3 beats V2 | **FAIL** — the list is `[owner]` only |
| 4 | reuses `groom.newest_gate()`, or states why not | **STATED** — reimplemented; and the reimplementation dropped the one thing that made the shared rule work (`base_id`) |
| 5 | security half intact (forged file, no GREEN, unreachable) | **PASS** |
| 6 | self-test covers multi-gate selection | **PASS in fixture, FAIL in reality** — fixture titles are exact ids |

## Clause 1 — the ALLOW, and what actually produced it (`01-live.log`, `02-prefix-control.log`)

Before the fix, same command on `0633743`:
```
$ python3 tools/board/merge_pr.py --pr 204
[merge_pr] PR #204: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head
[merge_pr] NOT MERGED (fail closed)          rc=1
```
After (`1b2fc86`):
```
[merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed
[merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)          rc=0
```
`--pr 213` → `ok: graded prefix d245ead.. matches current head d245eadf140b..` rc=0; `--pr 209` →
`b3540e5.. matches b3540e58569f..` rc=0. Clauses 1 and 2 as written are satisfied.

## Clause 3 — the failing cell: no verify card is ever a candidate (`04-regex.log`)

```
issues returned by state=all: 221

--- does the selector's regex match real verify-card titles? ---
  #221  match=False title='GB-110-V4 Verify gate-ordinal selection: --pr 204 must ALLOW on the V2 v'
  #219  match=False title='GB-110-V3 Verify the guard works: --pr 213 must ALLOW, pagination reache'
  #218  match=False title='GB-035-V2 Verify the NES emulator boots and plays a license-clean ROM in'
  #213  match=False title='GB-110-V Verify merge guard graded sha from newest GREEN (board API); fo'
--- AND groom.GATE_RE (the shared rule the card points at)? ---
  #221  GATE_RE(full title)=False   GATE_RE(base_id)=True   base_id='GB-110-V4'

--- which cards were considered for PR #204 / #211 (traced) ---
  PR #204: cards queried=[200]   chosen sha=3da727c
  PR #211: cards queried=[211]   chosen sha=d245ead
```
Eight of eight sampled verify cards are rejected by the selector's regex; the only card ever read is the
**owner** (`GB-106` #200 for PR #204). GB-106-V (#202) — the superseded gate this PR exists to demote —
was never even fetched.

So the card's own test of structuralness fails: for a parent carrying V/V2/V3/V4 the candidate list is
`[owner]`, not `[221, 219, 213, 212]`. It is not sorted by issue number either — it is not sorted at all.
The ALLOW for `--pr 204` came from #200's newest GREEN (my V2 verdict posted to the parent card as well),
which is luck, not the mechanism.

## Clause 4 — the shared rule, and the single word that broke the copy

`groom.GATE_RE = ^(GB-\d{3})-V(\d*)$` is fed **`base_id(title)`** (the leading token) and only then matches.
`merge_pr` copies the regex but feeds it the **full title** — hence the table above. I did not reuse
`newest_gate()` directly, and the reason is real: it takes a title→issue map and returns one ticket, with no
paging, no owner fallback, and no (ordinal, number) tie-break. But the copy must keep the `base_id`
normalisation, and that is exactly what it dropped.

**Constructive probe** (my own, uncommitted): matching `gb_v_re` against the leading token instead yields
```
  GB-106: ordinal-ordered candidates = [203, 202, 201]          (V2 → V → owner)
  GB-110: ordinal-ordered candidates = [221, 219, 216, 213, 212] (V4 → V3 → V2 → V → owner)
```
so V3 does beat V2 once the token is normalised — the fix is one `.split()[0]`.

## Clause 5 — security half (`05-security-and-test.log`)

```
forged local file (= head) vs board 1111…      -> graded_sha_of None   (refused)
honest local file (= board)                    -> the board's sha
FORGEJO_URL=http://127.0.0.1:9, matching file   -> None
no FORGEJO_TOKEN, matching file                 -> None
```
Unchanged and correct: the board wins, the file is only a must-match cache, silence fails closed.

## Clause 6 — the self-test has teeth, on a fixture that hides the bug

The multi-gate case passes and the comparator *is* guarded — reversing the sort makes it fail:
```
break: gate_orders.sort() (lowest first)  -> FAIL: the HIGHEST-ordinal gate's GREEN (GB-106-V2) wins … (got eeeeeeee)
                                            MERGE_PR_TEST: FAIL (1)   rc=1   [restored: PASS, tree clean]
```
But the fixture's titles are `{"number": 202, "title": "GB-106-V"}` and `{"number": 203, "title":
"GB-106-V2"}` — **exact ids with no description**, the one shape real cards never have. The comparator is
covered; the question "does this comparator ever run on a real title?" is not asked, so the suite is green
while the board is unaffected.

## What would make this GREEN

Normalise before matching — `m = gb_v_re.match((t or "").split()[0])` (or call
`groom.base_id(t)`), and add a fixture whose title is a **real** card title,
`"GB-106-V2 Verify the board-sourced graded sha: …"`, asserting the V2 candidate is selected. With that,
the clause-3 probe above returns `[221, 219, 216, 213, 212]` and the test would have caught this round.

## Reproduce

```
git worktree add /tmp/gb110v4 1b2fc86 && cd /tmp/gb110v4
python3 tools/board/merge_pr.py --pr 204        # ALLOW (via the owner card)
python3 tools/board/merge_pr.py --pr 213        # ALLOW
python3 - <<'PY'                                # the finding: real titles vs the selector's regex
import re; print(re.compile(r"^GB-(\d{3})-V(\d*)$").match("GB-110-V4 Verify gate-ordinal selection: …"))
PY
# -> None;  with .split()[0] -> matches, and candidates become [221, 219, 216, 213, 212]
```