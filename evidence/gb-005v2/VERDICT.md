# GB-005-V2 — gate PR #253 at 7e78ff2 (verifier run, 2026-10-30)

All six clauses run on this VM against the graded head. Nothing below is quoted
from the ticket; each block is this run's own stdout.

## Clause 1 — the lane-move refusal

Production, on GB-005 (card has no GREEN -V on this head):

```
$ BOARD_AGENT=wgames-verify/gate bash tools/board/lane-move.sh GB-005 "Merge Gate"
lane-move: REFUSED to move GB-005 to Merge Gate: REFUSE: GB-005 has no GREEN verdict -
a card cannot reach Merge Gate without a GREEN from its -V (GB-005); use --force with a
written reason to bypass
REAL_EXIT=1
```

ALLOW arm, on a scratch board (`sarah/gaming-program-vgate`, synthetic cards —
production cards are not test fixtures):

```
$ BOARD_REPO=sarah/gaming-program-vgate lane-move.sh GB-900 Merge Gate
lane-move: GB-900: ALLOW - newest verdict is GREEN; may move to Merge Gate
GB-900 #1: In Progress -> Merge Gate
exit=0

$ ... lane-move.sh GB-901 Merge Gate          # newest -V is RED
REFUSE: newest GB-901-V verdict is RED, not GREEN — ...            exit=1

$ ... lane-move.sh GB-902 Merge Gate          # no -V at all
REFUSE: GB-902 has no GREEN verdict ...                            exit=1
```

## Clause 2 — the refusal can provably fail

```
$ python3 tools/board/merge_gate_enforce_test.py
PASS: RED -> Merge Gate move refused
PASS: no verdict -> FAILS CLOSED (refused)
PASS: GREEN -> Merge Gate move allowed
PASS: RED refusal names the card (written reason)
PASS: sabotaged verdict source reports GREEN (the broken detector)
PASS: refusal STOPS refusing when the detector is broken (can fail -> not decoration) (reason='')
PASS: verdict source RESTORED after the sabotage (same callable)
PASS: merge helper refuses with no graded sha (fail closed)
PASS: merge helper refuses a GREEN that names a different head
[merge_gate_enforce_test] 3/3 passed
MERGE GATE ENFORCE TEST PASSED                                     exit=0
```

## Clause 3 — merge_pr.decide() refuses (run by me, no mocks)

```
[REFUSE] no graded GREEN ref at all
         REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head
[REFUSE] GREEN naming a DIFFERENT head
         REFUSE: graded 7e78ff2.. is not a prefix of the current head c08913fdeadb.. — refusing to merge a tree that was not graded
[ALLOW ] GREEN naming the head (allow arm)
         ok: graded prefix 7e78ff2.. matches current head 7e78ff2aabbc.. — merge allowed
[REFUSE] graded sha shorter than 7
[REFUSE] no head at all (unreachable API)
```

## Clause 4 — self-test

```
$ python3 tools/board/merge_gate_enforce.py --self-test
  PASS: GREEN -> allow
  PASS: RED -> refuse
  PASS: no verdict -> FAILS CLOSED
  PASS: garbage text -> refuse
MERGE_GATE_ENFORCE_SELFTEST: PASS - allow/refuse rule fails closed      exit=0
```

## Clause 5 — host_tests

```
$ make host_tests
[host-tests] done: 34/34 passed                                    HOST_EXIT=0
  PASS  tools/board/groom_test.py
  PASS  tools/board/merge_gate_enforce_test.py
  PASS  tools/board/merge_pr_test.py
```

`groom.newest_gate` (groom.py:165) and `groom.verdict_in` (groom.py:178) are present,
and `merge_pr` imports both (merge_pr.py:115-116). Note: `merge_pr_test.py` **fails on
`main`** (`merge_pr: error: unrecognized arguments: --head deadbeef`) and passes here —
this branch restores what main dropped, which is the commit's own claim.

## Clause 6 — two defects, both in the force path

`--force=<reason>` records the reason:

```
reason: reason via the equals form
```

The **documented** form `--force "reason"` (lane-move.sh header and its usage line)
does not:

```
$ lane-move.sh GB-902 "Merge Gate" --force "GB-005-V2 clause 6: verify the bypass is attributable"
**MERGE-GATE FORCE** (GB-005 override) — moved to a lane/merge path without a GREEN.\n- operator: wgames-verify/gate(clause1-test)\n- gid: GB-902\n- at: 2026-10-30T12:40:51Z\n- reason: forced override
```

Two separate problems in that one record:

1. **the reason is `forced override`**, not the string the operator passed — the
   argument parser only reads `--force=` (case `--force=*)`), so the space-separated
   form silently drops it. A bypass is attributable to an operator, but the *why* is
   gone, and the usage line advertises the form that loses it.
2. **the newlines are literal `\n`** — the record renders as one line with backslash-n
   escapes rather than a list.

Neither makes the bypass silent: operator, gid and timestamp are all recorded, and the
record is durable (read back from the API, comment id 17731).