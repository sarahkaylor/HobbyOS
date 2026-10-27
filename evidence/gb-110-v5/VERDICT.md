# GB-110-V5 (#222) — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 06:4xZ.
Verifies GB-110 (#211) / ship **PR #214 @ `6e9cbd2`**. Host-side T1 only; no CI consulted. I did not author
GB-110 or PR #214.

**Tree: `6e9cbd24ec77e667502932bb3c36a6c47409a00a`.**

## One line

The ordinal rule now fires on **real titles**: the cards are keyed by their bare id (`title.split()[0]`) and
`groom.newest_gate()` — the shared function, finally — picks the highest gate. `--pr 204` queries **#203
(GB-106-V2)** and allows on `3da727c…`; the parent card is no longer consulted at all. Putting V4's bug back
in makes the suite **fail** and the live guard refuse again, so the fix is guarded, not merely present.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | ordinal ordering fires on **real titles**, V2 beats V | **PASS** — full titles quoted, `newest_gate` → #203 |
| 2 | `--pr 204` ALLOWs `3da727c…` **by that mechanism** | **PASS** — candidates `[203]`, not `[200]` |
| 3 | `--pr 213` still ALLOWs `d245eadf140b…` | **PASS** |
| 4 | reuses `groom.newest_gate()`, or states why not | **PASS** — imported and called |
| 5 | security half intact | **PASS** |
| 6 | self-test uses **real card titles** | **PASS** — and it now catches V4's bug |

## Clauses 1–3 — the mechanism, on the board as it is (`01-ordering.log`)

Cards, with the titles they actually carry:
```
#203  closed  bare_id=GB-106-V2  title='GB-106-V2 Re-gate the merge guard: #194 must go red for the MISMATCH (e8c7dc89'
#202  closed  bare_id=GB-106-V   title='GB-106-V Verify the fail-closed merge guard: mismatch refused, no-sha refused,'
#201  closed  bare_id=GB-106-V   title='GB-106-V Verify merge_pr.py fail-closed guard: graded tree must equal current '
#200  closed  bare_id=GB-106     title='GB-106 A gate verdict names a tree, a PR names a head, and nothing here makes '

groom.newest_gate('GB-106', by_title) -> #203 'GB-106-V2'
```
GB-110 carries five gates and the selector ranks them by ordinal:
```
keys: GB-110-V, GB-110-V2, GB-110-V3, GB-110-V4, GB-110-V5
newest_gate('GB-110') -> #222 'GB-110-V5'
```
The guard's actual candidate walk (traced through `_forgejo`):
```
PR #204: candidates=[203]  sha=3da727c  allow=True
         ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed
PR #213: candidates=[215]  sha=d245eadf140b0a1f88c57354e40b815e8ee21275  allow=True
```
`[203]` is GB-106-V2 — the superseded V (#202) is skipped because it is not the newest gate, and the owner
card (#200) is not reached at all. That is the V4 finding closed by structure rather than by luck.

CLI, unchanged in behaviour for the honest path: `--pr 204` rc=0, `--pr 213` rc=0, `--pr 209` rc=0
(`b3540e5..` matches `b3540e58569f..`).

**Ordinal, not issue number.** On today's board the two orders coincide for every parent (0 disagreements),
so I tested the distinction the card warns about directly, in memory:
```
by_title = {GB-999-V2: #400, GB-999-V: #500, GB-999: #300}
newest_gate('GB-999') -> 400 GB-999-V2          verdict: ORDINAL
GATE_RE on a bare key: True   / on a full title: False
```

## Clause 4 — the shared function

`from groom import newest_gate as _newest_gate` and `gate = _newest_gate(f"GB-{gb}", by_title)`. The reason
`GATE_RE` works here and failed in V4 is the line above: it is applied to **bare-id keys**, exactly as in
`groom.py`. Fifth-round inline copy is gone.

## Clause 5 — security half (`02-ordinal-and-sec.log`)

```
forged local (= head) vs board 2222…   -> None   (refused)
honest local (= board)                 -> 2222…  (the board's sha)
dead board + matching local file       -> None
no token + matching local file         -> None
```

## Clause 6 — the fixture now uses real titles, and bites

Fixture strings: `"GB-106-V Re-gate the guard "`, `"GB-106-V2 Re-gate: #194 must go red "` — descriptions and
all. Re-introducing V4's bug (matching the whole title) now fails the suite **and** reproduces V4's live
behaviour:
```
break: k = t (whole title)  ->  FAIL: the HIGHEST-ordinal gate's GREEN (GB-106-V2) wins … (got None)
                                 MERGE_PR_TEST: FAIL (1)   rc=1
                                 $ merge_pr.py --pr 204  ->  REFUSE: GREEN names no graded sha   rc=1
restore                      ->  MERGE_PR_TEST: PASS   rc=0   (tree clean)
```
That is the property V4 lacked: the test fails on exactly the defect that shipped.

## One residual, stated (not a clause failure)

If `from groom import newest_gate` ever fails, `_newest_gate` is `None` and the code silently degrades to
`order = [owner]` — i.e. the V4 bug returns with no signal. Import and call both work today (the object is
the real function), so this does not affect the verdict; but failing closed loudly (or asserting the import)
would be cheaper than a sixth round.

## Reproduce

```
git worktree add /tmp/gb110v5 6e9cbd2 && cd /tmp/gb110v5
python3 tools/board/merge_pr.py --pr 204        # ALLOW, and the card queried is #203 (GB-106-V2)
python3 tools/board/merge_pr.py --pr 213        # ALLOW on d245eadf140b…
python3 tools/board/merge_pr_test.py            # PASS
# can-fail: match titles whole (V4's bug) -> MERGE_PR_TEST FAIL (1) and --pr 204 REFUSEs
```