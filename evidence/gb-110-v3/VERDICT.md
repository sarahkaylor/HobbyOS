# GB-110-V3 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-110-V3 (#219) · verifies GB-110 (#211) · ship = PR #214, branch
`ticket/110-guard-reads-real-verdicts` head **`340ff28`** (+57/−13, 2 files)
Gate tree = `origin/main` `937a517` × `340ff28` (`ort`, clean)
Local only — no CI consulted.

## VERDICT: RED

Clause 1 passes — and that is the first time this guard has ever allowed anything. But clause 2 fails,
and it fails on the same board that clause 1 passes on: **a card whose verify tickets were re-minted
still refuses**, because the search aborts at the first candidate card instead of moving on.

| # | Clause | Result |
|---|---|---|
| 1 | `--pr 213` ALLOWs and prints the sha read | **PASS** — `graded prefix d245ead.. matches current head d245eadf140b..` |
| 2 | pagination reaches all; a **closed** card's GREEN is still found | **FAIL** — paging and `state=all` work; the closed card's GREEN is discarded |
| 3 | 7-char shas parse from a real verdict body | **PASS** — `['3da727c', 'bab2bb2', '66f16bd']` |
| 4 | multi-sha selects this head, not `shas[0]` | **PASS** |
| 5 | security half intact | **PASS** |
| 6 | self-test uses a real verdict body | **PASS with a caveat** — real body shape, but the board reader is still stubbed |

### 1 — THE ONE COMMAND

    $ python3 tools/board/merge_pr.py --pr 213
    [merge_pr] PR #213: ok: graded prefix d245ead.. matches current head d245eadf140b.. — merge allowed
    [merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)                 rc=0

Two rounds of RED and this is the first ALLOW on a real PR. It is genuine: the sha came off the board, not
a flag or a file.

### 2 — the failing cell

Paging is real, `state=all` is real, the closed cards are visible:

    page 1: 50 issues (#219 … #170)
    page 2: 50 issues (#169 … #120)
    page 3: 50 issues (#119 … #70)
    page 4: 50 issues (#69 … #20)
    page 5: 19 issues (#19 … #1)
    total scanned: 219
    #200 present: True  state=closed   #203 present: True  state=closed
    #214 present: True  state=closed   #215 present: True  state=closed

and then, for a merged PR with a GREEN on its closed verify card:

    graded_sha_from_board(204) = None
    graded_sha_of(204)         = None

The cause is visible in the candidate order the guard walks, `[201, 202, 203, 200]`:

    #201: 1 matching 'VERDICT … GREEN'
       2026-10-26T19:03:46Z  '## VERDICT: GREEN — GB-106-V, PR #204 head `68982dc` …'
          shas=['68982dc', 'd706c8cd0714', 'd706c8c', …]  any head-prefix=[False, False, False, False]
    #203: 1 matching 'VERDICT … GREEN'
       2026-10-26T21:28:25Z  '**VERDICT — GREEN (GB-106-V2, round 2).** Re-gated on the new head **`3da727c`** …'
          shas=['3da727c', 'bab2bb2', '66f16bd', …]      any head-prefix=[True, False, False, True]

`#201` is an **older, superseded** GB-106 verify card whose GREEN names the previous head. The loop takes
the first card in `order`, finds a GREEN that does not name this head, and **returns None for the whole
search** instead of continuing to `#203`, which does name it:

```python
for num in order:
    ...
    for c in reversed(cs):
        if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", body, re.I):
            for sha in graded_shas_in(body):
                if head and head.startswith(sha):
                    return sha
            return None        # <- abandons every remaining candidate card
```

Re-minting a verify ticket is the normal shape in this program (`GB-106` has #201, #202, #203), so this is
not an edge case: the guard works on single-card tickets and refuses on re-minted ones. Clause 1 passes only
because GB-111 has exactly one verify card.

### 3 / 4 / 5 — the parts that do work

Clause 3, on the real `#203` GREEN body:

    graded_shas_in(#203 GREEN) -> ['3da727c', 'bab2bb2', '66f16bd', '3da727c']
    PR 204 head = 3da727ce691c08c0f46c2d69db24a03e10516fa9
    decide(head, shas[0]) -> allow=True  ok: graded prefix 3da727c.. matches current head 3da727ce691c..

Clause 4, base + head + merge-tree:

    graded_shas_in -> ['3da727ce691c…', '33333333…']
    the sha the guard would take (head-prefix rule): ['3da727ce691c…']        # the head, not shas[0]

Clause 5:

    forged cache 'f'*40 vs board -> graded_sha_of(204) = None
    unreachable board            -> graded_sha_from_board(204) = None

No fallback to the file anywhere in `graded_sha_of()`.

### 6 — the fixture

```python
check("deadbeefe0" in graded_shas_in("## VERDICT: GREEN\n**Tree that would merge:** deadbeefe0\n"),
      "reads an abbreviated (10-char-prefix) sha from a real GREEN body")
```

That is a real body shape (the canonical `**Tree that would merge:**` line, an abbreviated sha) rather than
`"c" * 40` — clause met. The caveat that matters: the GB-110 cases are **still** driven through
`mp.graded_sha_from_board = lambda pr, env=None: …`, so paging, `state=all`, candidate ordering and the
head-selection above are untested by the suite — which is exactly why the clause-2 defect passes the tests.

## Fix-forward

1. Do not abort at the first non-matching candidate: `continue` to the next card, or better, gather every
   candidate GREEN and pick the **newest GREEN comment** that names the head.
2. Test the reader itself — a fake `_forgejo` returning a paged issue list with a superseded `-V` card
   ahead of the live one — so this shape cannot regress silently.