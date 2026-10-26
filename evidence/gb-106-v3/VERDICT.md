# GB-106-V2 (round 2) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-106-V2 (#203) · verifies GB-106 (#200) · ship = branch `games/gb106` head **`3da727c`**
Gate tree `bab2bb2` = `origin/main` `66f16bd` × `3da727c` (`ort` merge, clean)
Delta on top of the round-1 head (`89acb0c`): `tools/board/merge_pr.py` (+94/-10), new
`tools/board/merge_pr.sh`, `Makefile` (+9)
Local only — no CI consulted.

## VERDICT: GREEN

Round 1's two failing cells are closed, and the three that already passed still pass.

| # | Clause | Result |
|---|---|---|
| 1 | refusal both directions, both shas named | **PASS** — allow rc=0 / refuse rc=1 |
| 2 | fails closed on silence | **PASS** — rc=1, "GREEN names no graded sha" |
| 3 | `#194` red for the mismatch; silence separately labelled | **PASS** |
| 4 | `--override`: reason required, **posted to the card**, mismatch named | **PASS** — names the pair, posts to #203 |
| 5 | the production path (call site) | **PASS** — `make merge_pr` → `merge_pr.sh` → `merge_pr.py --merge`, and the verb refuses without merging |
| 6 | self-test can fail | **PASS** — `FAIL (2)` rc=1 under a forced-equal break, restored |

### 1-3 — driven through the CLI (no `--merge`, so nothing could be merged)

    --- graded == head (this tree):
    [merge_pr] PR #204: ok: graded sha bab2bb28b576 equals current head — merge allowed
    rc=0

    --- graded != head:
    [merge_pr] PR #204: REFUSE: graded sha 7de6dde4f3f2 != current head bab2bb28b576 — refusing to merge a tree that was not graded
    [merge_pr] NOT MERGED (fail closed)                                    rc=1

    --- silence (no graded sha), measured WITHOUT a pipe:
    [merge_pr] PR #204: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head bab2bb28b576
    [merge_pr] NOT MERGED (fail closed)                                    silence-rc=1

    --- the #194 shape (merged e8c7dc89.., graded 7de6dde..):
    [merge_pr] PR #194: REFUSE: graded sha 7de6dde4f3f2 != current head e8c7dc89abcd — refusing to merge a tree that was not graded
    rc=1

The self-test keeps the two cases visibly apart (the V1 conflation):

    PASS: GREEN names no sha -> FAILS CLOSED (refuse)
    PASS: today's #194 incident (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch (REFUSE: graded sha 7de6dde4f3f2 != curre)

### 4 — the override now names what it overrides, and posts it

    $ python3 tools/board/merge_pr.py --pr 194 --head e8c7dc89abcd… --graded 7de6dde4f3f2… \
        --override "verifier exercise of the override path (GB-106-V2 gate, no real merge intent)" --card 203
    [merge_pr] OVERRIDE recorded (…/obj/gate/merge_pr_override/pr-194.reason.json) — graded 7de6dde4f3f20000… != head e8c7dc89abcd0000…
    [merge_pr] override posted to card #203 (comment 10531)
    [merge_pr] MERGE ALLOWED (written override)                            rc=0

**Disclosure:** comment 10531 on #203 is *this verifier's* exercise of the override path, not worker
output — it is the trace clause 4 asks for, and it names the mismatched pair. The JSON record carries
the reason, user, timestamp and head.

### 5 — the call site exists now

`Makefile`:

    3813:# tools/board/merge_pr.sh (guard --merge; refuses to merge a PR whose GREEN graded …
    3818:	bash tools/board/merge_pr.sh $(PR)

`tools/board/merge_pr.sh` is the wrapper (`merge_pr.sh <pr> [--head …]`, sources `~/.hermes/.env`,
exits non-zero if the guard refuses), and `merge_pr.py --merge` is the verb that only calls
`merge_pr_github()` **after** `decide()` allows. Its refusal path, run for real:

    $ python3 tools/board/merge_pr.py --pr 194 --head e8c7dc89abcd… --graded 7de6dde4f3f2… --merge
    [merge_pr] PR #194: REFUSE: graded sha 7de6dde4f3f2 != current head e8c7dc89abcd — refusing to merge a tree that was not graded
    [merge_pr] REFUSED TO MERGE (fail closed)                              rc=1

No merge call was reached — I did not let this gate merge anything.

**Residual, stated because it is not a repo fact:** the wrapper and target are the documented merge
verb, but whether the Coordinator actually merges through them lives in the operator's procedure, not in
the tree. What the tree now contains is a single call chain rather than a parallel checker, which is
what clause 5 can be held to.

### 6 — the self-test can fail

    $ sed -i 's/    if graded != head:/    if False and graded != head:/'
      FAIL: graded != head -> refuse
      FAIL: today's #194 incident … -> RED for the mismatch (ok: graded sha 7de6dde4f3f2 equals curre)
    MERGE_PR_TEST: FAIL (2)                                                rc=1

restored → `MERGE_PR_TEST: PASS`, rc=0 (first run of the day on this head: 7/7 PASS).