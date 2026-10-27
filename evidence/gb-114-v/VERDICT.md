**RED — GB-114-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #223 @ `86f8473`. Six of seven clauses pass; clause 4's stated falsifier fails, and it fails in the direction the card names as the risk.**

Evidence: `evidence/gb-114-v/` (self-test output, guard runs, probe sources, `--self-test` of check_verdicts). Trees: `wt-gb114v` @ `86f8473` and `wt-gb114v-main` @ `983da99` (pre-change). Local only; no CI is cited, used, or gated on.

## The failing check (clause 4)

Clause 4 requires: *"show that a body with neither phrasing resolves to `None` (a non-verdict line beginning \"Green…\" is not GREEN)"*.

`groom.verdict_in()` returns **GREEN** for ordinary English that begins with the word:

```
$ python3 evidence/gb-114-v/probe_guard.py
== clause 4: verdict_in on non-verdict bodies ==
  None   <- Greenfield
  GREEN  <- Green eggs and ham (line starts with Green)
  GREEN  <- Green, and then some.
  RED    <- RED herring
  None   <- mid-sentence
  GREEN  <- real GB-113-V opening
```

The cause is the second alternative of `VERDICT_RE` (`tools/board/groom.py:191-194`): `(?:^\s*(?:#+\s*)?\**\s*(GREEN|RED)\b)` — any line whose first token is GREEN/RED is a verdict. `\b` accepts a following space or punctuation; only a following letter (their own `"Greenfield"` case) is excluded.

## Why that is not cosmetic — the merge path

`graded_shas_in()` takes shas from any line containing `merge` (among other keywords), so one sentence supplies both halves:

```
$ python3 evidence/gb-114-v/probe_regression.py     # 86f8473
body: Green light to merge at 86f8473c0ffee — the diff is small.
  groom.verdict_in  -> 'GREEN'
  graded_shas_in    -> ['86f8473c0ffee']
```

`merge_pr.py:166-171` then does `is_green = (_verdict_in(body) == "GREEN")` and, if green, returns that sha; if it prefix-matches the head, `decide()` allows. **Pre-change that body was not a verdict at all** — `983da99:tools/board/merge_pr.py:133` was `if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", body, re.I):`, which this body does not match. So the widening converts a chat sentence into a merge-authorising GREEN naming the head: a tree that was never gated can merge.

The same reader drives `check_verdicts.py` and the lint's `newest_verdict`, so "Red flag: …" as a first line also flips a GREEN card's verdict — the failure is two-directional.

**Fix that would earn the clause:** require a delimiter or end-of-line after the marker (em dash, colon, comma, end of line — i.e. `(GREEN|RED)(?=\s*(?:—|–|-|:|,|\n|$))`), and/or require the bold/heading form; then add `"Green light to merge at <sha>"`, `"Green eggs and ham"` and `"RED herring"` to `groom.py --self-test` as must-be-`None` cases. The regex must also not treat a fenced code block as a verdict.

## What passed, quoted

**1. The real case.** `merge_pr.py --pr 222` (rc=0):
```
[merge_pr] PR #222: ok: graded prefix d21759c.. matches current head d21759c84acc.. — merge allowed
```

**2. It did not just widen everything.** #218 allows; #194 still refuses on a real mismatch, rc=1:
```
PR 218 rc=0 :: [merge_pr] PR #218: ok: graded prefix c7f1d9f.. matches current head c7f1d9f04204.. — merge allowed
PR 194 rc=1 :: [merge_pr] PR #194: REFUSE: graded 13ddb2d.. is not a prefix of the current head e8c7dc89bc01.. — refusing to merge a tree that was not graded
```
Note for the record: the board's newest GREEN for that ticket now names `13ddb2d`, not the `7de6dde` the card records — the refusal is a genuine graded/head mismatch either way, and the guard reaches it rather than reporting silence.

**3. Ambiguity refuses.** Self-test line `PASS: branch and title naming DIFFERENT tickets -> ambiguous, refuse`, and a constructed live call:
```
  (None, 'ambiguous:112=branch,113=title') <- games/gb112 + title GB-113
  ('113', 'title') <- ticket/222-waitstatus + title GB-113 (PR #222's real shape)
  (None, None) <- no id anywhere
```

**4. One helper, three call sites** — this half is satisfied:
```
merge_pr.py:116:from groom import verdict_in as _verdict_in
merge_pr.py:166:is_green = ((_verdict_in(body) == "GREEN") if _verdict_in
check_verdicts.py:68:from groom import verdict_in
check_verdicts.py:69:v = verdict_in(body)
groom.py:213:v = verdict_in(body)
```

**5. The wider net, measured.** `check_verdicts.py` on the live board: **51** at `86f8473` vs **50** at pre-change `983da99`, both `mismatched parents: 0`.

**6. No silent fallback.** The distinct line, on the production path with the ticket resolution stubbed to `None` (no PR is merged; the guard exits before any merge call):
```
[merge_pr] PR #9999: REFUSE: cannot resolve this PR to a board ticket (no gbNNN in the head branch, title or body) — the guard cannot know which GREEN governs it
[merge_pr] REFUSED TO MERGE (fail closed)
[probe] exit status = 1
```
No live PR exists whose branch, title and body all lack `gbNNN` (all 100 most recent PRs carry one in the title), so this is a stubbed call of the same branch, and I say so.

**7. Self-tests.**
```
groom self-test: 20 checks, 0 failed -- ALL PASSED
MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red
self-test result: fails=0
```

## Consequence

Clause 4 fails, so the card is **RED** and GB-114 returns to **In Progress** with this failing check named. Clauses 1, 2, 3, 5, 6 and 7 are met and should not be re-done. The clause-2 safety property is not established while the reader accepts a sentence: the widened net currently *can* authorise a tree no gate ever graded, which is exactly the outcome the verdict rule asked this gate to rule out.