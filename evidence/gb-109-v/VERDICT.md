# GB-109-V (#210) — VERDICT: **RED** (the guard is right; its "vacuous call is inexpressible" self-test cannot fail)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 23:1xZ.
Verifies GB-109 (#209) / ship **PR #209 @ `2c23ef8`**. Host-side T1 only, as the card says; no CI consulted.
I did not author GB-109 or PR #209.

## One line

Every prefix rule the card names is **implemented and works live** — 7-char prefix allows, <7 refuses,
non-prefix refuses, no-sha fails closed, #194 goes red — but the last clause, *"the vacuous `--head` call is
inexpressible"*, is only **asserted**: the self-test accepts **any** `SystemExit`, so when I re-introduce
`--head` and honour it in `main()` (the exact vacuous call the card exists to forbid) the self-test **still
prints PASS** while the live guard happily allows a merge from a forced head. A test that passes with the
defect present is not a check.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | self-test covers full-eq allow, != refuse, no-sha, no-head, #194 red | **PASS** |
| 2 | 7-char graded prefix of head → **ALLOW** | **PASS** — live on PR #204 |
| 3 | graded <7 → REFUSE; 7-char non-prefix → REFUSE | **PASS** — live |
| 4 | head comes from the API, never a flag; vacuous call **inexpressible** | **FAIL** — inexpressible in code, but the self-test cannot detect it becoming expressible |

## Clause 2/3 — the prefix rules, live (`02-live-guard.log`)

PR #204's API head is `3da727ce691c08c0f46c2d69db24a03e10516fa9`:
```
$ --graded 3da727c        (7-char prefix)
[merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce.. — merge allowed
[merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)          rc=0

$ --graded ffffff         (6 chars)
[merge_pr] PR #204: REFUSE: graded sha too short (6<7) — cannot be a verified prefix
[merge_pr] NOT MERGED (fail closed)                                           rc=1

$ --graded 3da727d        (7-char NON-prefix)
[merge_pr] PR #204: REFUSE: graded 3da727d.. is not a prefix of the current head 3da727ce..   rc=1

$ --graded 3da727ce691c08c0f46c2d69db24a03e10516fa9   (full)
[merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce.. — merge allowed  rc=0

$ --head x
merge_pr.py: error: unrecognized arguments: --head x                          rc=2
```
(An earlier run of mine pasted `3da727c6…` — one `e` short of the head — and the guard **refused** it. That
was my typo, and the refusal is the guard working; recorded so the record is not cleaner than reality.)

## Clause 4 — the failing cell: the self-test passes with the defect present

The check as shipped (`tools/board/merge_pr.py:138-147`):
```python
sysargv, sys.argv = sys.argv, ["merge_pr", "--pr", "1", "--head", "ff"]
try:
    main()
    check(False, "--head must be rejected by main()")
except SystemExit:
    check(True, "main() rejects --head (the vacuous flag call is inexpressible)")
```
`main()` calls `sys.exit(1)` on the ordinary "NOT MERGED" path, so **any** outcome raises `SystemExit` and
the check passes — it cannot tell "the parser rejected `--head`" (exit 2) from "the guard ran and refused"
(exit 1), let alone from "`--head` was accepted".

**Demonstrated.** I re-added `--head` and honoured it (`head = a.head or head_of_pr(a.pr)`), i.e. the exact
vacuous call (`04-breakB.log`):
```
break B applied: --head flag added and honoured in main()
--- self-test must now FAIL on the --head clause ---
selftest rc=0
  PASS: main() rejects --head (the vacuous flag call is inexpressible)
MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red

--- live: a forced --head is now accepted and allows the merge ---
[merge_pr] PR #204: ok: graded prefix 3da727c.. matches current head 3da727ce.. — merge allowed
rc=0
```
So the property "the vacuous call is inexpressible" has **no** working guard in the test suite: the code has
no `--head` today, and nothing would notice if it came back.

For contrast, the other clauses' guards *do* bite. Break A — replace the prefix comparison with exact
equality (`03-can-fail.log`):
```
  FAIL: graded 7-char prefix of head -> ALLOW (REFUSE: graded aaaaaaa..)
MERGE_PR_SELFTEST: FAIL (1)
--- live: a real 7-char prefix is now REFUSED ---  REFUSE … rc=1
```
Tree restored after both breaks (`git status --short` empty; `MERGE_PR_SELFTEST: PASS`).

## The fix (one line)

Assert the *parser* rejection, not "some exit":
```python
except SystemExit as e:
    check(getattr(e, "code", None) == 2, "main() rejects --head via the parser (exit 2, not 1)")
```
or capture stderr and require `unrecognized arguments: --head`. With that, break B fails the self-test —
which is what the card asks for.

## Reproduce

```
git worktree add /tmp/gb109v 2c23ef8 && cd /tmp/gb109v
python3 tools/board/merge_pr.py --self-test                      # PASS
python3 tools/board/merge_pr.py --pr 204 --graded 3da727c        # allow, rc=0
python3 tools/board/merge_pr.py --pr 204 --graded ffffff         # too short, rc=1
# the RED: add --head to the parser and honour it -> self-test still PASS, merge still ALLOWED
```