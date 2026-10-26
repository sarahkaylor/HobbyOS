# GB-100-V — finding: no merge request exists (not graded)

Card rule: *"If no PR exists when you claim this, post one line naming that and stop — do not grade a
branch, do not grade the card's prose. That is a real finding (the lane is mis-set), and it is what this
card is for."* That is the case.

Measured 2026-10-26 05:32–05:40Z, `origin/main` @ `c71cb87`:

| check | result |
|---|---|
| `git fetch origin 'refs/pull/*/head'` then search all 185 PR heads for `b450584` | **none contains it** |
| PR heads whose `origin/main...pr/N` diff touches `arch/x64/trap*`, `process.c`, `resume*` | **none** (last 25 PR heads scanned; the only resume-related PRs are #125/#126/#151/#170/#172/#173/#176/#177, all GB-057 and all older than the GB-100 split) |
| `git log --all --grep=GB-100` | plan/docs commits only (`ae0534c`, `567a95e`, `0085682`) plus the evidence commit `b450584` |
| `origin/hermes/gb100` @ `b450584` ("evidence: GB-100 findings — 0 resume-returns, 5 fatal-frame CPU halts …") | exists; diff vs main = `evidence/gb-100/fatal-lines.txt` + `evidence/gb-100/repro.log`, **+40 / −0 — evidence only, no code** |
| is that branch an ancestor of `origin/main`? | **no** |

So GB-100 is in `In Review` with no diff and no MR, and the branch carries only a findings log whose own
title says "RETURNS route unconfirmed". §4.2 step 2 puts the merge request before the review, so the lane
is unsupportable: **GB-100 → In Progress**, **GB-100-V → Done**.

Not graded, per the card: no reproduction quoted, no clause table, no RED/GREEN.