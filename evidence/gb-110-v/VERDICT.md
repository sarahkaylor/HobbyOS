# GB-110-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-110-V (#213) · verifies GB-110 (#211) · ship = branch `games/gb110` head `0633743`
(`tools/board/merge_pr.py` +88, `merge_pr_test.py` +19, 5 files, +102/−18)
Gate tree `04bc956` = `origin/main` `44d3cbb` × `0633743` (`ort`, clean)
Local only (T1, no QEMU) — no CI consulted.

## VERDICT: RED

The function is right and its entry point is not. `graded_sha_of()` now reads the board and fails
closed, but `main()` computes `graded = a.graded or graded_sha_of(a.pr)`, so **any caller with
`--graded` never consults the board at all** — a caller-controlled sha still permits a merge, which is
the class of defect this ticket exists to kill. And the test that is supposed to prove the board is the
source of truth **replaces `graded_sha_from_board` with a lambda**, so it cannot see the real read.

| # | Clause | Result |
|---|---|---|
| 1 | `merge_pr_test.py` incl. the GB-110 cases | **PASS as printed — hollow** (the three cases are stubbed) |
| 2 | forge-proof at the API boundary; sha from a GREEN, never a caller-controlled source | **FAIL** — `--graded` bypasses the board |
| 5 | break the board read → the test fails | **FAIL** — the test still passes |

### 2 — the failing cell, run against the live board

    ### board UNREACHABLE + caller-supplied --graded
    $ FORGEJO_URL=http://127.0.0.1:9 python3 tools/board/merge_pr.py --pr 204 --graded 3da727c
    [merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed
    [merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)                 rc=0

The board was not reachable and had nothing to say; the guard allowed anyway, on the caller's word. With
`--merge` that same line merges — `if a.pr and a.merge: graded = a.graded or graded_sha_of(a.pr)` — and I
did **not** run it, because it would have merged an ungated tree.

A forged local file is no better guarded:

    ### FORGED local cache naming a DIFFERENT tree (44d3cbb…), board names 3da727c
    $ printf '44d3cbb0…' > obj/gate/graded-204.sha
    $ python3 tools/board/merge_pr.py --pr 204 --graded 3da727c
    [merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head … — merge allowed      rc=0

The forged cache is irrelevant here only because the flag makes the cache irrelevant too — the decision
was taken without reading either.

The board-sourced path itself behaves correctly **when the flag is absent**:

    $ FORGEJO_URL=http://127.0.0.1:9 python3 tools/board/merge_pr.py --pr 204     # board down
    [merge_pr] PR #204: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head
    [merge_pr] NOT MERGED (fail closed)                                                   rc=1
    $ printf '44d3cbb0…' > obj/gate/graded-204.sha; python3 tools/board/merge_pr.py --pr 204   # forged cache
    [merge_pr] PR #204: REFUSE: GREEN names no graded sha — …
    [merge_pr] NOT MERGED (fail closed)                                                   rc=1

and the honest path allows:

    $ python3 tools/board/merge_pr.py --pr 204 --graded 3da727c
    [merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed

So the fix is real *behind* the flag and voidable *in front of* it. "The graded sha comes from the board's
newest GREEN" is false on the only path an operator actually types.

### 5 — the test cannot see the thing it is testing

I replaced the board read with the local file, exactly as the card asks:

```python
_lp = os.path.join(TREE, "obj", "gate", f"graded-{pr}.sha")
if os.path.exists(_lp):
    return open(_lp).read().strip() or None
```

    ### DELIBERATE BREAK — board read returns the local file
      PASS: a FORGED local cache disagreeing with the board -> FAIL CLOSED
      PASS: no GREEN on the board (or unreachable) -> FAIL CLOSED even with a local file
    MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red                    test-rc=0

It passes because the test never calls the real function:

```python
board_sha = "abcdef1234567890abcdef1234567890abcdef12"
_orig_fb = mp.graded_sha_from_board
mp.graded_sha_from_board = lambda pr, env=None: board_sha          # board GREEN names a sha
...
mp.graded_sha_from_board = lambda pr, env=None: None               # board has no GREEN / unreachable
```

All three GB-110 cases run against a stub. Nothing exercises `pr_gb_id()` (PR→GB via the head branch),
the `issues?state=open` listing, `graded_shas_in()`, or the newest-GREEN selection — which is why a
regression that returns the local file is invisible to the suite. The GB-109 half of the test is genuine;
the GB-110 half is a mock asserting on itself.

### Observed while reading (not my grade — belongs to #212 clause 4)

`graded_sha_from_board()` walks comments with `for c in reversed(cs)` and returns the first GREEN it meets;
there is no check that the GREEN postdates the head. It also queries `issues?state=open`, so once a verify
card is moved to Done its GREEN disappears and the guard fails closed for a legitimately-graded PR. Both
err safe, but the first means "newest GREEN overall", not "the verdict for this head".

## What would make this GREEN

1. Remove the bypass: on the merge path the sha must be `graded_sha_of(a.pr)` and nothing else. If
   `--graded` stays for the guard-only check, make it *narrow* — allow only when it is a prefix of the
   board's sha, and refuse when it disagrees or the board is silent.
2. Make the test exercise the real reader: inject the HTTP layer (a fake `FORGEJO_URL`/response) rather
   than replacing `graded_sha_from_board`, so PR→GB mapping, GREEN parsing and newest-GREEN selection are
   covered and the "return the local file" regression fails the suite.