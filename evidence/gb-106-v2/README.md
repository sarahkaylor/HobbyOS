# GB-106-V (#202) — gate evidence (VERDICT: RED on clause 3, narrowly and by measurement)

Ticket: GB-106-V (#202), weight S — second mint of the GB-106 verification
Verifies: GB-106 (#200), PR **#204** head `68982dc` (board files only: `merge_pr.py`, `merge_pr_test.py`)
Graded at `origin/main` **`fea33b0`**. Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).
Local, T1 only — no QEMU, no CI.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | refusal is real, both directions (equal → allow, differ → refuse naming both shas) | **PASS** |
| 2 | fails closed on silence (a GREEN naming no sha refuses) | **PASS** |
| 3 | the `#194` fixture goes red **for the incident's shape** (graded `7de6dde`, merged `e8c7dc89`) | **FAIL** |
| — | branch shape / merge ordering | **NOT RESPECTED at grading time** — stated below |

## 1 PASS — both directions, quoted
```
=== graded == head -> ALLOW
[merge_pr] PR #202: ok: graded sha d706c8cd0714 equals current head — merge allowed
[merge_pr] MERGE ALLOWED                                                      [exit 0]

=== graded != head -> REFUSE, naming both
[merge_pr] PR #202: REFUSE: graded sha 7de6dde00000 != current head d706c8cd0714 — refusing to merge a tree that was not graded
[merge_pr] NOT MERGED (fail closed)                                           [exit 1]
```

## 2 PASS — silence refuses
```
=== GREEN names no sha -> REFUSE
[merge_pr] PR #202: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head d706c8cd0714
[merge_pr] NOT MERGED (fail closed)                                           [exit 1]
```

## 3 FAIL — the `#194` fixture does not guard the incident it exists for

The card asks for a specific thing: *"That merge (graded `7de6dde`, merged `e8c7dc89`) must be a failing
case in the self-test."* What ships is this:

```python
merged = [194]  # today's GB-103 merge (actual PR #194)
for m in merged:
    grad = graded_sha_of(m)          # reads obj/gate/graded-194.sha -> None today
    ok, r = decide("DEADBEEF", grad) # synthetic head, not e8c7dc89
    check(not ok, f"today's PR #{m} has no recorded graded sha -> RED/fail-closed")
```
It goes red — but for **silence** (no sha file), not for the **mismatch** that actually happened. Neither
file contains the incident's shas:

```
$ grep -c "7de6dde\|e8c7dc8" tools/board/merge_pr.py tools/board/merge_pr_test.py
tools/board/merge_pr.py:0
tools/board/merge_pr_test.py:0
```

**Measured, not argued** (`evidence/gb-106-v2/clause3_probe.sh`): mutate `decide()` so a *mismatched*
graded sha is allowed — the incident's exact shape — and run the self-test:

```
=== mutation: a MISMATCHED graded sha is allowed (the incident's shape)
[mutant] mismatch now allowed
      PASS: graded==head -> allow
      FAIL: graded!=head -> refuse (ok: MUTANT allows a )        <- the generic assertion catches it
      PASS: no graded sha -> FAILS CLOSED
      PASS: no head -> refuse
      PASS: parses a 40-hex sha from a VERDICT/graded line
      PASS: no sha in VERDICT -> empty (fails closed upstream)
      PASS: today's PR #194 has no recorded graded sha -> RED/fail-closed   <- STILL GREEN
    MERGE_PR_SELFTEST: FAIL (1)
```
The `#194` line stays **green** under a guard that would have allowed the incident. So the fixture is not
guarding the incident — the generic mismatch assertion is, and it would be equally green if #194 had never
happened. That is the precise thing clause 3 was written to prevent.

To be fair to the implementation, **the rule itself is correct**: replayed through the shipped CLI, the
incident refuses.
```
$ python3 tools/board/merge_pr.py --pr 194 --head e8c7dc89… --graded 7de6dde…
[merge_pr] PR #194: REFUSE: graded sha 7de6dde00000 != current head e8c7dc890000 — refusing to merge a tree that was not graded
```
The defect is in the *fixture*, not the guard: two lines — add `{"pr": 194, "graded": "7de6dde…",
"head": "e8c7dc89…"}` and assert refuse — and the card's motivating incident is actually covered.

Note on scope: the parent DoD's literal wording ("self-tested against today's merges, including #194 which
must go red") *is* satisfied by the current line. This RED is on the card's stricter clause 3, which asks
for the incident's shape to be a failing case. If the Coordinator rules that the DoD wording governs, this
GREENs on a follow-up; I am not making that call myself.

## Branch shape / merge ordering — NOT RESPECTED at grading time

The card requires `#202` (GB-105) to land before `#204`, "or #204 would carry GB-105's code past its own
gate". At grading time (`fea33b0`):
* GB-105's classifier work is **not** on main (`git log origin/main --grep=GB-105` shows only the plan/mint
  commits, not the code);
* `git diff --stat origin/main pr/204` still shows **10 files, +280/−86**, including `tools/t3_tier.sh`,
  `tools/t3_classifier_coupling_test.py` and `tools/t3/t3_monitor_grading_test.sh`.
So merging #204 now would carry GB-105's 3 `tools/t3*` files past GB-105-V's gate. **Sequence #202 first.**
This is exactly the failure class GB-106 exists to prevent, so it belongs in the record rather than in a
merge note nobody reads.

## Verdict

**RED** on clause 3 only. Clauses 1 and 2 pass with live output; the guard's behaviour is right in every
direction I could push it; the self-test's `#194` case, however, cannot fail for the reason the incident
happened — proved by mutation, not by reading. Fix is two lines in the fixture, plus the merge ordering.