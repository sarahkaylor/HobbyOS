# GB-120-V — verdict: RED (the "two real consumers" are one command called twice; and the new Makefile wiring breaks `make host_tests`)

Subject: PR **#239**, head **`2271d1c`** — "tests: GB-120 — rig<->tier agreement test can actually fail
(GB-118 clause 8)". Fresh detached worktree of that head. Host-side only, no QEMU, **no CI consulted**.
Verifier: DEEP Verifier `wgames-verify-gb120v`. I authored neither GB-120 nor the change.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | Both **real** consumer handoffs run and agree (Makefile `run:` recipe **and** `t3_tier.sh`) | **RED** — the test calls the same `sh -c '<stage>'` twice and labels them A and B; neither `make run` nor `t3_tier.sh` is invoked, and on this head the Makefile `run:` recipe has **no** `RUN_STAGE` handoff at all |
| 2 | The test **fails** when the two diverge (perturb a side, show the failure) | **RED** — no such falsifier exists; measured: with `t3_tier.sh`'s handoff deliberately broken the test still reports `EVIDENCE RIG TEST PASSED` |
| 3 | `make host_tests` green, test named and running there | **RED** — `make host_tests` exits **2**, `Error 127`: the PR's own wiring runs `./evidence_rig_test` as a binary that is never built |

## 1 — "Consumer A" and "Consumer B" are the same expression

`tools/evidence_rig_test.py:58-61`:
```
        # Consumer A — the Makefile `run:` recipe handoff (ROOT-cwd + sh -c RUN_STAGE).
        ra = _run(f'ROOT="{root}" sh -c \'{stage}\'', env)
        # Consumer B — t3_tier.sh T3_STAGE handoff (ROOT="$ROOT" sh -c "$T3_STAGE").
        rb = _run(f'ROOT="{root}" sh -c \'{stage}\'', env)
```
Identical command, identical runner, identical arguments — the only difference is the comment above
each line. The `make run|t3_tier` strings in the file are all in prose (lines 7, 11, 60); no invocation
of either appears anywhere in the test.

And the handoff clause 1 names does not exist on this head:
```
$ grep -n "^run:" -A 2 Makefile
2465:run: $(TARGET) disk.img
2466-	$(QEMU_CMD)
$ git merge-base --is-ancestor 35f1fb41 HEAD && echo ancestor || echo absent
GB-118 fix NOT in this branch
```
So the test could not exercise the Makefile recipe even if it tried: `RUN_STAGE` lands with GB-118, which
this branch does not carry. That is a scheduling problem for the card (it is gated against a base that
lacks the thing it tests), not one the test can paper over.

## 2 — the falsifier is asserted, not run — and I ran one

Clause 2 says a falsifier that is only asserted is exactly the defect GB-120 exists to remove. The
test's "falsifiable" check overwrites the fixture with a different payload and asserts the extract
differs — that proves `mcopy` works, not that two consumers disagree.

The falsifier that matters, run here: break the tier's handoff in place —
```
182:  ROOT="$ROOT" sh -c "false && $T3_STAGE" || { echo "T3_${ARCH}: STAGE FAILED …"; exit 2; }
```
— and re-run the gate:
```
$ make evidence_rig_test
...
PASS: both real consumers execute the stage (ra=0 rb=0)
PASS: SAME fixture is guest-visible after the stages (rig & tier agree)
PASS: agreement test is FALSIFIABLE (perturbed fixture caught)
Tests run: 7, failed: 0
EVIDENCE RIG TEST PASSED            RC=0
```
A broken tier leaves the test green: it never reads `t3_tier.sh`. (Reverted afterwards; the worktree's
`git status` is clean.)

## 3 — `make host_tests` fails, because of this PR

The PR adds both a target that runs the python and a recipe line that tries to execute it as a binary
(`05-makefile-diff.patch`):
```
EVIDENCE_RIG_TEST = evidence_rig_test
$(EVIDENCE_RIG_TEST): tools/evidence_rig_test.py tools/evidence_rig.py
	@python3 tools/evidence_rig_test.py
...
host_tests: … $(AUDIO_STUB_TEST) $(EVIDENCE_RIG_TEST)
	$(HOST_RUN) ./$(EVIDENCE_RIG_TEST)
```
`./evidence_rig_test` is never built, so:
```
PASS: agreement test is FALSIFIABLE (perturbed fixture caught)
Tests run: 7, failed: 0
EVIDENCE RIG TEST PASSED
timeout: failed to execute process: No such file or directory (os error 2)
make: *** [Makefile:3657: host_tests] Error 127
RC=2
```
The parent Makefile has no `EVIDENCE_RIG_TEST` at all (`git show HEAD~1:Makefile | grep -c EVIDENCE_RIG_TEST`
→ `0`), so this is introduced by the PR, not inherited.

## Also worth naming

Two checks pass vacuously on this head:
```
PASS: build_arg_parser not yet on main (pending GB-118)  (skipped)
PASS: build_stage_cmd not yet on main (pending GB-118)  (skipped)
```
Calling a `hasattr` miss a PASS is the pattern the program keeps paying for; they should be reported as
skips the gate can see, or the card should be gated on a base that carries GB-118.

## What would make this GREEN

1. Gate it on a base that **has** GB-118 (`35f1fb41` or later), so `RUN_STAGE` and `build_stage_cmd`
   exist and clause 1 is even expressible.
2. Make the two legs *actually different*: leg A `make -n`/`make run` (or the recipe's exact line via a
   stub QEMU), leg B `bash tools/t3_tier.sh <arch>` (or its exact line). If the QEMU leg is reduced, say
   so and show the reduction still fails when the tier side is perturbed (the card's clause 4).
3. Perturbation must be *of a consumer*, then the test must go red — quote the failing run, not the
   passing one.
4. Drop `$(HOST_RUN) ./$(EVIDENCE_RIG_TEST)` or build a real binary; `make host_tests` must exit 0.

## Reproduce

```
git worktree add /tmp/w120 --detach 2271d1c && cd /tmp/w120
make evidence_rig_test                       # green
sed -i '182s/.*/  ROOT="$ROOT" sh -c "false \&\& $T3_STAGE" || exit 2/' tools/t3_tier.sh
make evidence_rig_test                       # still green -> the test never reads the tier
git checkout -- tools/t3_tier.sh
make host_tests                              # Error 127 on ./evidence_rig_test
```