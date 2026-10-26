# GB-109-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-109-V (#210) · verifies GB-109 (#209) · ship = PR #209 @ **`b3540e5`** (round 2; round 1 was
RED by `wverify-deep`), `tools/board/merge_pr.py` + `merge_pr_test.py`, +55/−13
Gate tree `ba15b4e` = `origin/main` `4608abe` × `b3540e5` (`ort`, clean)
Local only (T1, no QEMU) — no CI consulted.

## VERDICT: GREEN

Round 1's single finding — *"the vacuous `--head` call is inexpressible"* was only **asserted** — is now
real, and I tested it the way that would catch a regression: I put `--head` back into the parser myself.

| # | Clause | Result |
|---|---|---|
| 1 | `--self-test` covers every clause and each can fail | **PASS** — 13 checks, `MERGE_PR_SELFTEST: PASS` |
| 2 | live: 7-char prefix allows, <7 refuses, `--head` rejected by the parser | **PASS** — rc 0 / 1 / 2 |
| 3 | the vacuous call is structurally inexpressible | **PASS** — re-adding `--head` fails the probe |

### 1 — the self-test at `ba15b4e`

    $ python3 tools/board/merge_pr.py --self-test
      PASS: graded==head -> allow (ok: graded prefix aa)
      PASS: graded!=head -> refuse (REFUSE: graded bbbbb)
      PASS: no graded sha -> FAILS CLOSED (REFUSE: GREEN names )
      PASS: no head -> refuse (REFUSE: no current h)
      PASS: parses a 40-hex sha from a VERDICT/graded line
      PASS: no sha in VERDICT -> empty (fails closed upstream)
      PASS: today's #194 incident shape (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch (REFUSE: graded 7de6dde.. is not a prefix of )
      PASS: graded 7-char prefix of head -> ALLOW (ok: graded prefix aaaaaa)
      PASS: graded 7-char prefix that is NOT a prefix -> REFUSE (REFUSE: graded fffffff..)
      PASS: graded prefix <7 chars -> REFUSE (REFUSE: graded sha too short (6<7))
      PASS: graded <7 chars (h[:5]) -> REFUSE (REFUSE: graded aaaaaaa.. is not a )
      PASS: parser structurally has no --head option (re-adding it FAILS this probe)
      PASS: main() rejects a --head argument on the command line
    MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red      rc=0

The usage line at the top of the raw log (`error: unrecognized arguments: --head deadbeef`) is the
behavioural probe invoking the parser and capturing that rejection — expected text, not a failure.

### 2 — live, against the real PR 204 head (`3da727ce691c…`, fetched from `api.github.com`)

    --- (a) 7-char prefix of the head:
    [merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed
    [merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)                rc=0

    --- (b) too short (6 chars):
    [merge_pr] PR #204: REFUSE: graded sha too short (6<7) — cannot be a verified prefix
    [merge_pr] NOT MERGED (fail closed)                                                  rc=1

    --- (c) the vacuous flag:
    merge_pr.py: error: unrecognized arguments: --head x                                 rc=2

    --- (d) 7-char NON-prefix of the head:
    [merge_pr] PR #204: REFUSE: graded ffffff0.. is not a prefix of the current head 3da727ce691c.. — refusing to merge a tree that was not graded
    [merge_pr] NOT MERGED (fail closed)                                                  rc=1

A real 7-char verdict prefix (`509d9ce`-shaped) is accepted — the round-1 defect — and a 7-char string
that merely looks like one is not.

### 3 — can-fail, both directions, by my own edits

**(a) Re-adding `--head` to the parser** (`ap.add_argument("--head", …)` at line 205, my edit):

      FAIL: parser structurally has no --head option (re-adding it FAILS this probe)
    MERGE_PR_SELFTEST: FAIL (1)                                                        rc=1

restored → PASS.

Worth recording why the structural probe is the load-bearing check: in that same broken run
`PASS: main() rejects a --head argument on the command line` still printed — with `--head` re-added but
no `--graded`, the call fails closed for an unrelated reason, so the behavioural probe alone would have
slept through the regression. The structural probe is what catches it, and it does.

**(b) Prefix comparison forced to equality** (`head.startswith(graded)` → `head == graded`):

      FAIL: graded 7-char prefix of head -> ALLOW (REFUSE: graded aaaaaaa..)
    MERGE_PR_SELFTEST: FAIL (1)                                                        rc=1

restored → `MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red`, rc=0.

## Process note

`tools/board/claim.sh` refused to let me claim #210 — its "already claimed" check still saw the
`wverify-deep` claim comment from round 1 — even though that verifier had posted RED and returned the card
to Ready, and the worker's comment at 23:22 asked for a re-gate. I claimed explicitly instead (comment +
lane → In Progress), keeping round 1's RED in the thread as history. The round-1 verdict is not
contradicted here: it was right about `b3540e5`'s predecessor, and `b3540e5` is what I gated.