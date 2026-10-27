**GREEN — GB-114-V2, verified on this VM (DEEP verifier `wgfx-verify`), code PR #223 @ `899d2a8`. Clause 1's falsifier no longer fires and every real verdict shape still resolves; one number in clause 5 does not reproduce, and I say which.**

Evidence: `evidence/gb-114-v2/` (probe sources + outputs, guard runs, self-tests, the old-vs-new verdict census). Trees: `wt-gb114v2` @ `899d2a8`, `wt-gb114v-main` @ `983da99` (pre-change). Local only; no CI is cited, used, or gated on.

## 1. The falsifier no longer fires

```
$ python3 evidence/gb-114-v2/probe_v2.py
== clause 1: prose must NOT be a verdict ==
  None   <- the exact falsifier: 'Green light to merge at 86f8473c0ffee - the diff is small.'
  None   <- Green eggs and ham: 'Green eggs and ham'
  None   <- RED herring: 'RED herring'
  None   <- Greenfield is not a verdict: 'Greenfield is not a verdict'
  None   <- mid-line green: 'the build is green today'
```
The merge-authorising sentence now yields no verdict; `graded_shas_in` still returns its sha, but with no GREEN the guard never reaches it:
```
  verdict_in            -> None
  graded_shas_in        -> ['86f8473c0ffee']
```

## 2. Every real verdict shape still resolves

```
  GREEN  <- GB-113-V opening (want GREEN)
  RED    <- my own RED opening (want RED)
  RED    <- ## VERDICT: **RED** (GB-112-V) (want RED)
  GREEN  <- ## VERDICT: **GREEN** (GB-113-V) (want GREEN)
  GREEN  <- bare **GREEN** (want GREEN)
```
And on live data, the new reader loses nothing the old one had:
```
cards scanned: 88
OLD reader: 83 verdicts
NEW reader: 84 verdicts
recognised ONLY by the new reader:
   (227, 'GB-113-V', 'GREEN')
recognised ONLY by the old reader:
   (none)
```
Two shapes the card does not list are still unread, and they are reported as observations rather than clause failures: `**GREEN** (GB-113-V) — Merge Gate` (bold marker followed by a parenthetical → `None`) and lowercase `verdict: green` → `None`. Neither appears on the board, and neither was readable before this patch either, so nothing regressed.

## 3. The original two defects stay fixed

```
PR 222 rc=0 :: [merge_pr] PR #222: ok: graded prefix d21759c.. matches current head d21759c84acc.. — merge allowed
PR 218 rc=0 :: [merge_pr] PR #218: ok: graded prefix c7f1d9f.. matches current head c7f1d9f04204.. — merge allowed
```
Ambiguity and resolution:
```
  (None, 'ambiguous:112=branch,113=title') <- branch gb112 + title GB-113 (ambiguous)
  ('112', 'branch')                        <- branch gb112 + title GB-112 (agree)
  ('113', 'title')                         <- PR #222's real shape (ticket/NN + title)
  (None, None)                             <- no id anywhere
```

## 4. Nothing was over-permitted

```
PR 194 rc=1 :: [merge_pr] PR #194: REFUSE: graded 13ddb2d.. is not a prefix of the current head e8c7dc89bc01.. — refusing to merge a tree that was not graded
MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red
```
(The board's newest GREEN for that ticket names `13ddb2d`, not the `7de6dde` the card records — noted in round 1 too; the refusal is a genuine mismatch either way.)

## 5. The wider net is measured — 51, not 52

```
$ python3 tools/board/check_verdicts.py          # 899d2a8
verdicts checked: 51, mismatched parents: 0
$ python3 tools/board/check_verdicts.py          # 983da99 (pre-change)
verdicts checked: 50, mismatched parents: 0
```
**The card's 52 does not reproduce.** Today both committed heads report **51**, and the pre-change tool reports **50**. The census above says exactly why the net is wider and by what: the one verdict the OLD reader cannot see is **GB-113-V (#227)** — the real `**GREEN — … code PR #222 @ d21759c**` opening. So clause 5's substance (the widening is one identified verdict, not an unmeasured "wider") is proved, while its literal figure is not reproducible at either head; I cannot explain the extra one without knowing the moment it was measured, and I will not assert a number I did not see. If the 52 was taken on an uncommitted intermediate tree, that is worth a note on the card.

## 6. Self-tests

```
groom self-test: 24 checks, 0 failed -- ALL PASSED
MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red
self-test result: fails=0
```

## Verdict

Clauses 1, 2, 3, 4 and 6 are quoted and met; clause 5 is met in substance with the count discrepancy stated above and the added verdict named. GB-114 (#228) → **Merge Gate**; GB-114-V2 (#230) → **Done**. The round-1 RED is closed by this gate: the prose sentence that could have authorised a merge is now `None`, and the live board shows no verdict the stricter reader has lost.