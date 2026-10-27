# GB-110-V4 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-110-V4 (#221) · verifies GB-110 (#211) · ship = PR #214 head **`1b2fc86`**
Gate tree `2bde146` = `origin/main` `937a517` × `1b2fc86` (`ort`, clean)
Local only — no CI consulted.

## VERDICT: RED

`--pr 204` does now ALLOW and names `3da727c`, which is what the card asks for. It allows for the **wrong
reason**: the new ordinal rule matches **no real board title**, so nothing is ordered at all — the walk falls
back to the parent card, which happens to carry the right GREEN yesterday's verifier cc'd there.

| # | Clause | Result |
|---|---|---|
| 1 | `--pr 204` ALLOWs naming `3da727c`, with before/after quoted | **PASS as an outcome, not by the mechanism claimed** |
| 2 | `--pr 213` still ALLOWs `d245ead` | **PASS** — also via the parent fallback |
| 3 | ordinal ordering is structural, not incidental | **FAIL** — `gate_orders=[]` on the real board |
| 4 | reuses `groom.newest_gate()`, or states why not | **FAIL** — fourth inline implementation, no stated reason |
| 5 | security half intact | **PASS** |
| 6 | self-test covers multi-gate selection with real titles | **FAIL** — fixtures use bare titles that never occur |

### 1 — before and after, quoted

    before (9435ad2): [merge_pr] PR #204: REFUSE: graded 68982dc.. is not a prefix of the current head 3da727ce691c..
    after  (1b2fc86): [merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed

    $ python3 tools/board/merge_pr.py --pr 213
    [merge_pr] PR #213: ok: graded prefix d245ead.. matches current head d245eadf140b.. — merge allowed     rc=0

### 3 — the failing cell: the ordinal rule is dead code on real titles

The shipped matcher is anchored at the end of the title:

    '^GB-(\d{3})-V(\d*)$'  <-  'GB-106-V Verify merge_pr.py fail-close'          match=False
    '^GB-(\d{3})-V(\d*)$'  <-  'GB-106-V2 Re-gate the merge guard: #194 must'    match=False
    '^GB-(\d{3})-V(\d*)$'  <-  'GB-111-V Verify t4 probe guards socketpair/p'    match=False
    '^GB-(\d{3})-V(\d*)$'  <-  'GB-106 A gate verdict names a tree'              match=False

Only a bare title matches — and no card on this board is titled that way:

    'GB-106-V'  -> match=True ordinal=1
    'GB-106-V2' -> match=True ordinal=2

So the candidate list the shipped code actually builds is empty, and the walk is the parent card alone:

    GB-106: gate_orders=[]  owner=200  walk=[200]
    GB-111: gate_orders=[]  owner=214  walk=[214]

The allow comes from those two parents carrying a GREEN at all:

    #200 GB-106 parent: 2 GREEN comment(s), newest 2026-10-26T21:28:26Z
    #214 GB-111 parent: 1 GREEN comment(s), newest 2026-10-27T00:55:53Z

That is incidental in exactly the sense clause 3 forbids. Had #200 not been cc'd (verifiers copy parent cards
as a courtesy, not by rule), the guard would fall back to `return None` and refuse again; had the parent's
newest GREEN been an older one, it would have refused naming that stale sha. The ordinal branch never executes
on this board.

The arithmetic of the ordering itself is right — it just never sees an ordinal:

    [(3, 300), (2, 202), (1, 201)] -> highest ordinal first: (3, 300)
    [(3, 90),  (2, 202), (1, 201)] -> still ordinal-first:   (3, 90)      # V3 numbered below V2

### 4 — the fourth implementation, again

`groom.py:newest_gate()` and `GATE_RE = ^(GB-\d{3})-V(\d*)$` already encode this rule. The new code
reimplements it inline as `gb_v_re = re.compile(r"^GB-(\d{3})-V(\d*)$")` with a comment saying it is "the same
lesson groom.py's newest_gate() encodes" — same lesson, different regex, and no statement of why the shared one
could not be used. The drift is not hypothetical: this regex cannot match real titles, and `groom.py`'s
normalisation step (it matches on a derived key, not a raw title) is exactly the part that was not carried
over.

### 6 — why the suite is green

```python
mp._forgejo = lambda api, env=None: ([…] if "comments" in api else
    [{"number": 201, "title": "GB-106: owner "},
     {"number": 202, "title": "GB-106-V"},
     {"number": 203, "title": "GB-106-V2"}])
check(chosen == ord_head, "the HIGHEST-ordinal gate's GREEN (GB-106-V2) wins over the superseded V")
```

The multi-gate case is the right idea, but the titles are bare `GB-106-V` / `GB-106-V2` — the one form the
board never uses. A fixture with `"GB-106-V2 Re-gate the merge guard: #194 must"` would have failed
immediately.

### 5 — security half, unchanged and correct

    forged cache 'f'*40 -> graded_sha_of(204) = None
    unreachable board   -> None
    head from API only  -> 3da727ce691c08c0f46c2d69db24a03e10516fa9

## Fix-forward

1. Match the ordinal with a boundary, not end-of-string: `^(GB-\d{3})-V(\d*)\b` — or reuse `groom.GATE_RE`
   with the same key normalisation `newest_gate()` relies on, and say in the code why if not.
2. Fixtures must carry real titles (with the trailing description), for both the multi-gate and the
   parent-only cases.
3. Then `--pr 204` should ALLOW **because GB-106-V2's GREEN was selected**, which is the property worth
   having; I will check that the walk lists 203 first when this comes back.