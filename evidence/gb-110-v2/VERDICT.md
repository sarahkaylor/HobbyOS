# GB-110-V2 — post-merge gate on `0633743` as landed (VERIFIER, deep, swarm-farm-1)

Ticket GB-110-V2 (#216) · grades GB-110 **as landed**: `0633743` on `origin/main` (PR #211, squashed,
merged 2026-10-27T00:58Z), now `edd60fd` with 0633743 an ancestor
`tools/board/merge_pr.py` unchanged since 0633743 (`git log --oneline -1 -- tools/board/merge_pr.py`)
Local only — no CI consulted.

## VERDICT: RED

The security half is sound and the **usefulness half does not work at all**: on this board the guard
cannot read a single real verdict, so it refuses every honest merge. Two independent causes, each
sufficient on its own.

| # | Clause | Result |
|---|---|---|
| 1 | forgery dead, naming the board's sha | **PASS on refusal** — refuses; the message does not name the board's sha |
| 2 | silence fails closed | **PASS** |
| 3 | honest path end-to-end on the real board | **FAIL** — merged PR #213 refuses |
| 4 | multi-sha GREEN selects the head | **PARTIAL** — parser returns the head on a synthetic body, but the guard takes `shas[0]` unconditionally, and on real bodies the list is empty |
| 5 | self-test runs against a real verdict body | **FAIL** — still `"VERDICT GREEN graded sha " + "c"*40` |
| 6 | no silent fallback to the local file | **PASS** — `graded_sha_of()` never falls back |

### 3 — the failing cell, and why (two causes, measured, no stubs)

    $ python3 tools/board/merge_pr.py --pr 213
    [merge_pr] PR #213: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head
    [merge_pr] NOT MERGED (fail closed)                                                    rc=1

PR #213 is merged, its head is `d245eadf140b…`, and a GREEN exists for it. Probing the real path:

**(a) the issue scan sees 50 of 63 open issues.** `graded_sha_from_board()` asks for
`/issues?state=open&limit=300`; Forgejo caps that at 50 and the code never pages:

    issues returned for limit=300: 50
    first three: [(216, 'GB-110-V2 Post-merge gate…'), (213, 'GB-110-V Verify merge guard…'), (211, 'GB-110 The merge guard…')]
    last three:  [(45, 'GB-045 Performance pass…'), (41, 'GB-041 [DEEP] Doom…'), (39, 'GB-039 Game launcher…')]
      #200 present in scan: False
      #203 present in scan: False
      #214 present in scan: False
      #215 present in scan: False
    page 2 (limit=50): 13 issues; first #35
    total open per API (paging sum): 63

So `pr_gb_id(213) = '111'` is correct, and then `candidates: []` — GB-111's cards are on page 2, which
nothing requests. Closed cards are invisible by design too, so a verify ticket moved to Done takes its
GREEN out of reach permanently.

**(b) even a visible verdict yields no sha.** `graded_shas_in()` accepts only 40-hex, while real verdicts
name 7-char prefixes (that is GB-109's own rule). Fed two real GREEN bodies from this board:

    issue #203 GREEN comment 2026-10-26T21:28:25Z: 0 sha(s) parsed -> []
    issue #74  GREEN comment 2026-10-26T23:08:14Z: 0 sha(s) parsed -> []
    guard would use shas[0] = None

A verdict body structured as base / head / tree does yield a sha:

    graded_shas_in -> ['22222222…']            # the head, correctly
    guard would use shas[0] = '22222222…'

— but that only helps a verdict that happens to write a bare 40-hex sha line, and then `shas[0]` is taken
regardless of which sha it is. **Known limitation, named as the card asks:** with several 40-hex shas in a
GREEN, the guard uses the first, not the one matching the head; and with the abbreviated shas real verdicts
actually contain, it uses none.

### 1 / 2 / 6 — the security half does hold

    ### forged local cache naming a different tree
    [merge_pr] PR #213: REFUSE: … NOT MERGED (fail closed)                                  rc=1
    ### board unreachable, matching local file present
    [merge_pr] PR #213: REFUSE: … NOT MERGED (fail closed)                                  rc=1

That is correct behaviour, and it is also why nothing else shows: the guard refuses everything, so a
forgery test cannot distinguish "the forgery was caught" from "nothing was read at all". Clause 1's
"naming the board's sha" is not met either — when the cache disagrees, `graded_sha_of()` returns `None`
and the caller prints `GREEN names no graded sha`, never the board's sha.

`graded_sha_of()` has no fallback path to the file:

    board = graded_sha_from_board(pr, env)
    if board is None:
        return None                       # no GREEN / GREEN-no-sha / board unreachable
    ...
    if local is not None and local != board:
        return None                       # forged cache that contradicts the board -> refuse
    return board

(Separately: the `--graded` flag still short-circuits the board entirely at `main()` — `graded = a.graded
or graded_sha_of(a.pr)` at lines 314/330/342 — which is the RED already posted on #213. Not this card's
clause 6, but it is on the same lines and on `main`.)

### 5 — the fixture is still synthetic

    check(40 in [len(s) for s in graded_shas_in("VERDICT GREEN graded sha " + "c" * 40)], …)

That is a hand-built 40-hex line, not a verdict body. It passes while every real body on the board parses to
nothing, which is exactly the gap the clause was written to close.

## Fix-forward (this card's own rule: the work becomes a normal implementation card)

1. Page the issue listing (`?limit=50&page=N` until empty), and stop depending on `state=open` for the
   verdict lookup — read the card by number once it is known, whatever its state.
2. Accept abbreviated shas the way `decide()` already does (7–40 hex), and select the sha that prefixes
   the head rather than `shas[0]`.
3. Put a captured real verdict body in the fixture, so the suite fails when the parser stops understanding
   what verifiers actually write.
4. Remove the `--graded` short-circuit on the merge path (already RED on #213).