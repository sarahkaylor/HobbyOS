# GB-110-V (#212) — VERDICT: **RED** (the board read works; it cannot read 77 of 96 real GREENs, including the newest)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 00:2xZ.
Verifies GB-110 (#211) / ship **PR #211 @ `0633743`**. Host-side T1 only; no CI consulted. I did not author
GB-110 or PR #211.

## One line

The forgery defence is real — a local `graded-<pr>.sha` that contradicts the board yields `None`; a dead
board or an absent token yields `None` even when a matching file is present; and the self-test fails when
the board read is broken. But the **input** is wrong in the other direction: `graded_shas_in()` only reads a
sha from lines containing `VERDICT|graded|grading|tested|head`, and on this board that matches **19 of 96**
GREEN verdict comments. For PR #209 — whose newest GREEN (posted 20 minutes ago) names exactly the current
head `b3540e58569f…` — the guard reads `None` and **refuses a merge a real GREEN authorises**. That is
GB-109's defect ("the guard refuses real verdicts") reintroduced at the layer this card was minted to fix.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | forged local file contradicting the board → refuse, naming the board's sha | **PASS (unit)** — live demo blocked by clause 3 |
| 2 | no GREEN / unreachable board → refuse even with a matching file | **PASS** — live, dead port and no-token |
| 3 | the honest path still works: a real verdict sha against a matching head **allows** | **FAIL** — 77/96 real GREENs unreadable; PR #209 refused |
| 4 | uses the *newest* GREEN; an older GREEN cannot authorise a newer head | **PARTIAL** — safety holds via the prefix check; "newest" is card order, not time (code quoted) |
| 5 | self-test can fail when the board read is broken | **PASS** — `MERGE_PR_TEST: FAIL (1)` |
| 6 | no silent fallback to the local file | **PASS** — code quoted |

## Clause 3 — the failing cell (`02-green-readability.log`)

```
GREEN verdict comments scanned: 96
  graded_shas_in found a sha : 19
  graded_shas_in found NOTHING: 77
  unreadable e.g. card #211 2026-10-26T23:49:22Z: 'Implementation: `graded_sha_of()` = `graded_sha_from_board()` …'
  unreadable e.g. card #210 2026-10-26T23:39:37Z: '**VERDICT — GREEN (GB-109-V, round 2).** …'

--- the exact comment the guard must read for GB-109-V (#210) ---
GREEN header line: **VERDICT — GREEN (GB-109-V, round 2).** Round 1's finding …
graded_shas_in -> []
line carrying the graded tree: '**Tree that would merge:** `b3540e58569fbbd1ac9de77680e8dcbfb78d9486`'
```
And end to end, on real PRs (`01-probe.log`):
```
PR #211: branch->GB=110 head=0633743f8475… board_sha=None  decide -> REFUSE: GREEN names no graded sha
PR #209: branch->GB=109 head=b3540e58569f… board_sha=None  decide -> REFUSE: GREEN names no graded sha
PR #204: branch->GB=106 head=3da727ce691c… board_sha=None  decide -> REFUSE: GREEN names no graded sha
```
PR #209 is the case that matters: its board GREEN names `b3540e58…`, the head is `b3540e58…`, and the guard
still refuses — because the sha sits on a line that says **"merge"**, a word the filter does not include.

The self-test passes all three GB-110 cases because its fixture is written *in the readable style*. A
fixture that used a real GREEN comment (say the GB-109-V one) would have caught this immediately.

## Clause 1 — forgery is dead, at unit level (`03-clause12.log`)

Board read stubbed to `1111…`, local file forged to name the head `b3540e58…`:
```
graded_sha_of -> None    (None = refused)
decide() -> (False, 'REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head')
```
and with an honest cache equal to the board, `graded_sha_of -> 1111…` (the board's sha, not the file's).
**The live version of this test is impossible on today's tree** — not because the code is wrong but because
no real card yields a readable sha, which is clause 3's failure. I say so rather than dress the unit test up
as a live one.

## Clause 2 — silence fails closed, live (`03-clause12.log`)

With a local file present and **matching** the head:
```
FORGEJO_URL=http://127.0.0.1:9   -> graded_sha_from_board None, graded_sha_of None,
                                   decide → REFUSE: GREEN names no graded sha
no FORGEJO_TOKEN at all          -> graded_sha_of None
```
Both refuse. This is the property the card exists for, and it holds.

## Clause 5 — the suite can fail (`04-can-fail.log`)

Breaking `graded_sha_of` so an absent board falls back to the local file:
```
  FAIL: no GREEN on the board (or unreachable) -> FAIL CLOSED even with a local file
MERGE_PR_TEST: FAIL (1)      rc=1
```
restored: `MERGE_PR_TEST: PASS`, rc=0, `git status --short` empty.

## Clause 4 — "newest" is card order, not time (code, not a run)

```python
for it in issues:                      # cards in API order
    if t.startswith(gb_v):  order.insert(0, it["number"])     # the -V card first
    elif t.startswith(gb_t): order.append(it["number"])
for num in order:
    for c in reversed(cs):                                    # newest comment first
        if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", body, re.I):
            shas = graded_shas_in(body); return shas[0] if shas else None
```
So the `-V` card's newest GREEN always wins over a *newer* GREEN on the parent card, and nothing checks
that the GREEN postdates the head. **Safety still holds** — a stale GREEN names an old sha, and
`decide()`'s prefix rule refuses it against a newer head — but the failure mode is "refuses a good merge",
and `shas[0]` picks the *first* 40-hex on the *first* matching line, so a GREEN whose evidence link precedes
its graded sha can supply the wrong sha. Both are availability bugs, not forgery holes; I note them because
the card asked to hear them if the parser was "newest GREEN overall".

## Fix direction (not applied)

1. Make the parser read the *house* lines, minimally `merge` alongside `VERDICT|graded|head`, and prefer the
   sha on the VERDICT line or a `graded-sha:` marker — then re-run the census and expect ~96/96, or a named
   refusal ("GREEN has no machine-readable sha") instead of a silent `None`.
2. Have the verdict poster emit a machine-readable line (`graded-sha: <40hex>`) so this stops depending on
   prose.
3. Put a **real** GREEN comment in `merge_pr_test.py` as the fixture, so the readable-style synthetic case
   cannot pass while the board's actual comments do not.

## Reproduce

```
git worktree add /tmp/gb110v 0633743 && cd /tmp/gb110v
python3 tools/board/merge_pr_test.py                       # PASS (fixture is in the readable style)
python3 tools/board/merge_pr.py --pr 209                   # REFUSE: GREEN names no graded sha (guard only)
python3 ~/.hermes/cache/scratch/gb110_green_readability.py # 96 GREENs -> 19 readable, 77 not
```