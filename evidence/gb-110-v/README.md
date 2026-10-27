# GB-110-V (#212) — gate evidence (VERDICT: RED on clauses 3 and 4)

Ticket: GB-110-V (#212). Verifies GB-110 (#211), PR **#211** head **`0633743`** (unchanged since the
00:32Z RED — checked before gating). Rows host-side, T1, no QEMU, no CI.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).

## Clause table

| # | clause | result |
|---|---|---|
| 1 | forgery is dead — a local file naming the head cannot permit a merge | **PASS** |
| 2 | silence fails closed — no readable board sha refuses even with a matching local file | **PASS** |
| 3 | the honest path still works — a real verdict sha against a matching head allows | **FAIL** — real GREEN naming `b3540e5` is refused |
| 4 | it picks the verdict for *this* head (two-GREEN case) | **FAIL** — stops at the newest GREEN; `shas[0]` is not the head |
| 5 | the self-test can fail — break the board read, test goes red | **PASS** — mutation caught at the right assertion |
| 6 | no silent fallback to the local file | **PASS** — code quoted, and the mutant proves a fallback would be caught |

## 1 + 2 PASS — a forged cache cannot merge, and silence refuses

```
$ echo b3540e58569fbbd1ac9de77680e8dcbfb78d9486 > obj/gate/graded-209.sha   # = PR #209's exact head
$ python3 tools/board/merge_pr.py --pr 209
[merge_pr] PR #209: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head d…  [exit 1]
```
A local file naming the *exact current head* does not get a merge. That is the card's headline property and
it holds.

## 3 FAIL — a real GREEN that names the head is refused

PR #209's head is `b3540e58569f…`; ticket #210's GREEN says
`**Tree that would merge:** b3540e58569fbbd1ac9de77680e8dcbfb78d9486`. The guard, on that same board:

```
$ python3 tools/board/merge_pr.py --pr 209
[merge_pr] PR #209: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head
[merge_pr] NOT MERGED (fail closed)                                                       [exit 1]
```

## 4 FAIL — and here is exactly why (measured, not inferred)

`graded_sha_from_board()` walks the card's comments newest-first and **returns on the first GREEN**, giving
up if that one has no parseable sha:

```python
for c in reversed(cs):                     # newest first
    if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", body, re.I):
        shas = graded_shas_in(body)
        return shas[0] if shas else None   # <-- stops here; never falls back
```

Ticket #210 carries **two** GREENs, and that is the whole failure (`07_two_greens.log`):

```
[0] 2026-10-26T23:39:30Z  OLDEST
    graded_shas_in -> ['ef3673fc8e84…', 'b3540e58569f…']
    names: **Tree that would merge:** `b3540e58569fbbd1ac9de77680e8dcbfb78d9486` …
[1] 2026-10-26T23:39:37Z  NEWEST <- the tool stops here
    graded_shas_in -> []   <-- no 40-hex sha, so the tool returns None
```

Two independent defects, both visible in that output:

1. **It stops at the newest GREEN and does not fall back.** The newest GREEN (`23:39:37Z`) writes the tree
   as an abbreviated sha (`b3540e5`) — the form **GB-109 made legal input** — so `graded_shas_in()`, which
   matches only `\b[0-9a-f]{40}\b`, returns nothing, and the guard refuses a merge the older GREEN on the
   same card authorises.
2. **`shas[0]` is not necessarily the head.** Even on the older body that *does* parse, the first sha is
   `ef3673fc8e84…` (an evidence-branch commit), with the head second. Had the tool used that body, it would
   have produced a *false mismatch* rather than a pass.

So clause 4's question — "confirm the guard uses the newest [GREEN] and that an older GREEN cannot authorise
a newer head" — has a worse answer than either option: the newest GREEN is used, but a GREEN that names the
tree in the abbreviated form GB-109 supports is invisible to it, and the sha it does read may be the wrong
one on the line.

## 5 PASS — the self-test can fail

```
=== as shipped:                     merge_pr_test.py exit=0   MERGE_PR_TEST: PASS
=== mutation: silent fallback to the local file installed:
    FAIL: no GREEN on the board (or unreachable) -> FAIL CLOSED even with a local file
    MERGE_PR_TEST: FAIL (1)                                  [exit 1]
=== restored:                       exit=0
```
The mutation is caught by the assertion that owns it, not by a crash.

## 6 PASS — no silent fallback in the shipped code

```python
board = graded_sha_from_board(pr, env)
if board is None:
    return None                       # no GREEN / GREEN-no-sha / board unreachable
local = ...                           # cache only
if local is not None and local != board:
    return None                       # forged cache that contradicts the board -> refuse
return board
```
The local file is only ever a must-match cache; there is no path where a missing or unreachable board
yields the file's value. Confirmed by the clause-5 mutation being detectable.

## Verdict

**RED** on clause 3 and clause 4. The forgery defence — the card's headline — is real and I could not
break it. But the guard now refuses a merge that a real GREEN on the board authorises, which is GB-109's
defect reintroduced one layer down, and the two-GREEN case shows two distinct mechanisms: a newest-first
walk that gives up instead of falling back, combined with a 40-hex-only parse that cannot see the
abbreviated shas GB-109 explicitly legalised.

A fix that would satisfy both: scan GREENs newest-first but *continue* past any without a parseable sha
until one yields a value, accept ≥7-char prefixes, and pick the sha from the line that actually names the
tree rather than the first sha in the body.