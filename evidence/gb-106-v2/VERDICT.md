# GB-106-V2 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-106-V2 (#203) · verifies GB-106 (#200) · ship = PR #204 head `89acb0c`
Gate tree `cf235c5` = `origin/main` `66f16bd` × `89acb0c` (`ort` merge, clean)
Own work, verified against the merge-base: **2 files, +192** — `tools/board/merge_pr.py`,
`tools/board/merge_pr_test.py`
Local only — no CI consulted.

## VERDICT: RED

Clauses 1, 2, 3 and 6 pass, and the V1 finding is genuinely fixed: the `#194` fixture now goes red for
the **mismatch**, and the silence case is a separately labelled assertion. Clauses **4** and **5** fail:
the override leaves a local file and names no mismatch, and **nothing in the tree calls the guard** — it
is a parallel checker, not the production merge path.

| # | Clause | Result |
|---|---|---|
| 1 | refusal both directions, naming both shas | **PASS** |
| 2 | fails closed on silence | **PASS** |
| 3 | `#194` fixture red for the mismatch; silence separately labelled | **PASS** |
| 4 | `--override` leaves a trace: non-empty reason, **posted to the card**, mismatch named | **FAIL** — local JSON only; posts nothing; names no mismatch |
| 5 | it is the production path (show the call site) | **FAIL** — no call site exists |
| 6 | self-test can fail (break the comparison) | **PASS** — `FAIL (2)` rc=1, restored |

### 1-3 — driven directly against the shipped module (not the worker's transcript)

    decide(head=cf235c500000, graded=cf235c500000) -> allow=True
         ok: graded sha cf235c500000 equals current head — merge allowed
    decide(head=7de6dde00000, graded=cf235c500000) -> allow=False
         REFUSE: graded sha cf235c500000 != current head 7de6dde00000 — refusing to merge a tree that was not graded

    decide(head=cf235c500000, graded=None) -> allow=False
         REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head cf235c500000

    decide(head=merged e8c7dc800000, graded=7de6dde00000) -> allow=False
         REFUSE: graded sha 7de6dde00000 != current head e8c7dc800000 — refusing to merge a tree that was not graded
    both shas named in the reason: True

The test's own labels keep the two cases apart, which is what V1 conflated:

    PASS: GREEN names no sha -> FAILS CLOSED (refuse)
    PASS: today's #194 incident (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch (REFUSE: graded sha 7de6dde4f3f2 != curre)

### 4 — the failing cell: `--override`

    (a) empty reason:  rc=2   (rejected, but via argparse's generic "need --pr …" message)
    (b) written reason: rc=0
        [merge_pr] OVERRIDE recorded (…/obj/gate/merge_pr_override/pr-194.reason.json); merge allowed
        {"pr": 194, "reason": "re-tested the merged tree by hand", "by": "sarah", "at": …, "head": "cf235c5…"}

The reason is required and recorded — but **nothing is posted to the card** (the tool makes no board
call; the JSON is local), and **the mismatch is not named** in anything it emits. Worse, the override
branch does not call `decide()` at all:

    if a.override:
        path = record_override(a.pr, a.override)
        print(f"[merge_pr] OVERRIDE recorded ({path}); merge allowed")

so an operator overriding never sees which graded/head pair they are overriding — exactly the record
clause 4 exists to leave.

### 5 — the failing cell: no call site

    $ grep -rn "merge_pr" . | grep -v "^./tools/board/merge_pr" | grep -v "^./third_party" | grep -v "^./obj"
    ./gaming.md:412: | GB-106 | … with merge_pr.py refusing to merge until it passes …
    ./evidence/gb-106-v2/g1-selftest.log:1: ### G-all: merge_pr_test.py as shipped …

That is the whole tree: the tool is referenced by the plan row and by nothing else. No Makefile target,
no board script, no merge wrapper invokes it. It is a checker that *could* be used, not the path a merge
goes through — the clause's explicit distinction.

### 6 — the self-test can fail

    $ sed -i 's/    if graded != head:/    if False and graded != head:/'
      FAIL: graded != head -> refuse
      FAIL: today's #194 incident … -> RED for the mismatch (ok: graded sha 7de6dde4f3f2 equals curre)
    MERGE_PR_TEST: FAIL (2)                                          rc=1

restored → `MERGE_PR_TEST: PASS`, rc=0.

## What would make this GREEN

1. Wire it into the merge path: the board's merge verb (or the operator's merge wrapper) must call
   `merge_pr.py --pr <n> --head <sha>` and refuse on non-zero — with the call site shown, so the guard
   is not a side tool.
2. Make `--override` print — and post to the card — the recommendation it is overriding: compute
   `decide()` first, include `graded … != head …` in both the message and the comment, and only then
   record the reason.
3. Say in the tool's help what "posted to the card" means if it is deliberately manual; otherwise the
   clause has no referent.