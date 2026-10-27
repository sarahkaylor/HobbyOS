# GB-110-V3 (round 2) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-110-V3 (#219, round 2) · verifies GB-110 (#211) · ship = PR #214 head **`9435ad2`**
Gate tree = `origin/main` `937a517` × `9435ad2` (+74/−12, 2 files)
Local only — no CI consulted.

## VERDICT: RED

Clause 1 still passes. Clause 2 is still the failure, one layer deeper: the closed card's GREEN *is* now
returned instead of `None`, but the guard returns the **oldest** verify card's GREEN, so a re-minted ticket
refuses a merge whose newest GREEN names its head.

| # | Clause | Result |
|---|---|---|
| 1 | `--pr 213` ALLOWs and prints the sha read | **PASS** — `graded prefix d245ead.. matches current head d245eadf140b..` |
| 2 | a closed card's GREEN authorises the merge | **FAIL** — the superseded card's GREEN wins |
| 3 | 7-char shas parse from a real verdict body | **PASS** — `['3da727c', 'bab2bb2', '66f16bd']` |
| 4 | multi-sha selects this head | **PASS** |
| 5 | security half intact | **PASS** |
| 6 | self-test uses a real verdict body | **PARTIAL** — real body shape, but the new case is still a one-card mock |

### 1 — still good

    $ python3 tools/board/merge_pr.py --pr 213
    [merge_pr] PR #213: ok: graded prefix d245ead.. matches current head d245eadf140b.. — merge allowed
    [merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)                 rc=0

### 2 — the failing cell

    $ python3 tools/board/merge_pr.py --pr 204
    [merge_pr] PR #204: REFUSE: graded 68982dc.. is not a prefix of the current head 3da727ce691c.. — refusing to merge a tree that was not graded
    [merge_pr] NOT MERGED (fail closed)                                                   rc=1

`68982dc` is **not** this PR's graded tree — it is the graded sha of a *superseded* verify card. The newest
GREEN on #203 (2026-10-26T21:28Z) names `3da727c`, which is this head:

    graded_sha_from_board(204) = 68982dc
    graded_shas_in(#203 GREEN) -> ['3da727c', 'bab2bb2', '66f16bd', '3da727c']   # head-prefix: True

Cause — the candidate walk:

    GB-106 candidate order: [(201, 'GB-106-V …'), (202, 'GB-106-V …'), (203, 'GB-106-V2 …'), (200, 'GB-106 …')]
    GB-111 candidate order: [(215, 'GB-111-V …'), (214, 'GB-111 …')]

Issues arrive newest-first, and every `-V` match is `order.insert(0, …)`, so with more than one verify card
the **oldest** ends up first. The loop then stops at that card: it returns `shas[0]` when nothing matches the
head, without ever looking at #203. #201's GREEN predates #203's by 2½ hours and names the previous head.

Round 1's failure modality changed (silent `None` → a refusal that names a sha), which is an improvement in
the error, not in the outcome: an authorised merge is still refused, and the refusal names a tree that was
never graded for this head. GB-106 is the ordinary shape here (three verify cards); GB-111, with one, is why
clause 1 passes.

### 3 / 4 / 5 — unchanged and correct on this head

    graded_shas_in(#203 GREEN) -> ['3da727c', 'bab2bb2', '66f16bd', '3da727c']
    decide(head, shas[0]) -> allow=True
    multi-sha body -> the sha the guard takes is the head, not shas[0]
    forged cache 'f'*40 vs board -> None          unreachable board -> None

### 6 — the new test is still a one-card mock

```python
mp._forgejo = lambda api, env=None: ([{"body": "## VERDICT: GREEN\n**Tree that would merge:** " + "a"*40 + "\n"}]
                                     if "comments" in api else [{"number": 7, "title": "GB-110-V"}])
found = mp.graded_sha_from_board(209)
```

It asserts "found is not None", which is exactly the weaker property the round-1 fix delivered. With a single
issue in the fixture it cannot see the ordering defect at all — the two-card case is the one that fails.

## Fix-forward

1. Walk **all** candidate cards; collect every GREEN with its comment timestamp and return the sha of the
   **newest GREEN that names the head** (fall back to the newest GREEN's sha only for the refusal message).
   At minimum, `continue` past a card whose GREEN does not name the head instead of returning from it.
2. Build the ordering the way the board really looks: a mock `_forgejo` serving a paged issue list with two
   `-V` cards where the older one comes first, only the newer naming the head.

## Process note

This is the third RED on GB-110 (#212, #216, #219-r1) and the fourth round overall. The pattern — a guard
whose own verification keeps failing on shapes the suite cannot see — is what `DECISION-005` (#173) was minted
to decide. I am not the one to decide it; recording it here so the decision has this data point.