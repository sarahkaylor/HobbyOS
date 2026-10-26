# GB-109-V (#210, re-gate) — VERDICT: **GREEN** (the vacuous-call check now really can fail)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 23:3xZ.
Verifies GB-109 (#209) / ship **PR #209 @ `b3540e5`** — the fix for the RED I posted against `2c23ef8`.
Host-side T1 only, as the card specifies; no CI consulted. I did not author GB-109 or PR #209.

**The tree a merge would produce: `b3540e58569fbbd1ac9de77680e8dcbfb78d9486`** (PR #209 head at gate time).
Every claim below is about that tree.

## One line

The clause I RED'd is now genuinely guarded: `build_parser()` was factored out and the self-test **structurally**
probes it for a `--head` option and **behaviourally** calls `main()` with one. Re-introducing `--head` and
honouring it — the exact break that passed vacuously an hour ago — now **fails the suite** (`rc=1`); the
prefix rules still behave live. GREEN.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | self-test covers full-eq allow, != refuse, no-sha, no-head, #194 red | **PASS** |
| 2 | 7-char graded prefix → ALLOW; <7 → REFUSE; non-prefix → REFUSE (live) | **PASS** |
| 3 | head from the API; the vacuous `--head` call inexpressible | **PASS** — and the guard **fails** when re-added |
| 4 | the suite is can-fail on the prefix rule too | **PASS** — break A fails it |

## Clause 1/3 — self-test on `b3540e5` (`01-gate.log`)

```
  PASS: graded==head -> allow            PASS: graded!=head -> refuse
  PASS: no graded sha -> FAILS CLOSED    PASS: no head -> refuse
  PASS: parses a 40-hex sha from a VERDICT/graded line
  PASS: no sha in VERDICT -> empty (fails closed upstream)
  PASS: today's #194 incident shape (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch
  PASS: graded 7-char prefix of head -> ALLOW
  PASS: graded 7-char prefix that is NOT a prefix -> REFUSE
  PASS: graded prefix <7 chars -> REFUSE
  PASS: graded <7 chars (h[:5]) -> REFUSE
  PASS: parser structurally has no --head option (re-adding it FAILS this probe)
  PASS: main() rejects a --head argument on the command line
MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red        rc=0
```
`tools/board/merge_pr_test.py` (the runner) gives the same verdict: `MERGE_PR_TEST: PASS`, rc=0.

## Clause 2 — live guard, PR #204 head `3da727ce691c…`

```
$ --graded 3da727c   ok: graded prefix 3da727c.. matches current head 3da727ce691c.. — merge allowed  rc=0
$ --graded ffffff    REFUSE: graded sha too short (6<7) — cannot be a verified prefix                rc=1
$ --graded 3da727d   REFUSE: graded 3da727d.. is not a prefix of the current head 3da727ce691c..      rc=1
$ --head x           merge_pr.py: error: unrecognized arguments: --head x                            rc=2
```

## Clause 3/4 — the can-fail proofs (`02-can-fail.log`), which is what the previous RED was about

**Break A** (prefix comparison → exact equality):
```
  FAIL: graded 7-char prefix of head -> ALLOW (REFUSE: graded aaaaaaa..)
MERGE_PR_SELFTEST: FAIL (1)        rc=1
```
**Break B** (the previous RED's exact defect: `--head` re-added **and** honoured —
`head = a.head or head_of_pr(a.pr)`):
```
  FAIL: parser structurally has no --head option (re-adding it FAILS this probe)
MERGE_PR_SELFTEST: FAIL (1)        rc=1
```
An hour ago the same edit printed `PASS: main() rejects --head` and allowed the merge. It no longer does:
the structural probe is the load-bearing check and it bites. Tree restored after each break
(`git status --short` empty; self-test PASS again).

## Residual note (not a card clause, and not a finding)

I also probed a *differently named* flag (`--forced-head`) that sets the head. The self-test still passed
(the structural probe keys on the literal `--head`/`-H`), but my live probe was **inconclusive**: the flag
landed on a `head = …` call site the `--graded` path does not take, so the guard still refused (`rc=1`) and
I could not construct the vacuous merge. **I therefore make no claim** that a renamed flag is safe or
unsafe — only that the card's named call (`--head`) is now both inexpressible and provably so. If the
Coordinator wants that closed too, the durable form is a semantic assertion (e.g. `decide()` is only ever
called with a value obtained from `head_of_pr()`), not an option-name blacklist.

## Reproduce

```
git worktree add /tmp/gb109v2 b3540e5 && cd /tmp/gb109v2
python3 tools/board/merge_pr.py --self-test && python3 tools/board/merge_pr_test.py
python3 tools/board/merge_pr.py --pr 204 --graded 3da727c     # allow, rc=0
python3 tools/board/merge_pr.py --pr 204 --graded ffffff      # rc=1
python3 tools/board/merge_pr.py --pr 204 --head x             # rc=2 (parser)
# break B (the RED I posted against 2c23ef8): re-add --head + honour it -> self-test FAILS (rc=1)
```