# GB-118-V3 — verdict: GREEN (clause 8 only) at `9866b7de`

Subject: PR **#231**, graded head **`9866b7de`** (`9866b7d` — "rig<->tier agreement test must reach BOTH
real consumers, not one runner twice"). Scope: **clause 8 only**, as the card instructs; clauses 1-7 were
graded at `35f1fb41` under GB-118-V2 (#258) and are not re-run here. Host-side, no QEMU boot, **no CI
consulted**. Verifier: DEEP Verifier `wgames-verify-gb118v3`.

## Clause matrix

| # | Item | Result |
|---|---|---|
| 1 | Both legs are distinct real consumers; resolved command lines quoted | **PASS** — leg A is `make` executing the **live-read** recipe; leg B is the tier's `ROOT="$ROOT" sh -c "$T3_STAGE"` invocation (details and the one caveat below) |
| 2 | Falsifier run by me: break `t3_tier.sh`'s `$ROOT`, test fails | **PASS** — `FAIL: t3_tier.sh still invokes T3_STAGE via ROOT=$ROOT sh -c (live read)`, rc=1; restored → pass |
| 3 | Drift caught on the **Makefile** side too | **PASS** — `FAIL: Makefile run recipe still drives RUN_STAGE via ROOT=$(CURDIR) shell (live read)`, rc=1; restored → pass |
| 4 | `make host_tests` green at this head | **PASS** — `Checks run: 482`, `Checks failed: 0`, `TEST EXIT: 0`, with `PASS tools/evidence_rig_test.py` in the run |
| 5 | The stubbing is disclosed and harmless | **PASS** — `QEMU_CMD = @:` is quoted, and the recipe line is read from the real `Makefile` (proved by item 3) |

## 1 — the two legs, resolved

```
LEG A recipe line (read live from Makefile):
  $(if $(RUN_STAGE),@echo "=== run: stage ==="; ROOT="$(CURDIR)" $(SHELL) -c "$$RUN_STAGE" || { echo "run: STAGE FAILED - refusing to boot a stale image"; exit 2; },)
LEG A runner: make -C <throwaway dir> run   (throwaway Makefile carries that verbatim line; QEMU_CMD = @:)
LEG B line:  ROOT="$ROOT" sh -c "$T3_STAGE" || { echo "T3_${ARCH}: STAGE FAILED - refusing to boot a stale image"; exit 2; }
LEG B runner: sh -c 'ROOT="$ROOT" sh -c "$T3_STAGE"'
```
They are no longer one call twice: leg A goes through GNU make (so `$$RUN_STAGE` undergoes the real make →
shell expansion, and `ROOT="$(CURDIR)"` is assigned by make), leg B replays the tier's own line with
`ROOT="$ROOT"` expanded by a shell. Baseline:
```
PASS: make run recipe executes RUN_STAGE (leg A consumer) (exit=0)
PASS: tier T3_STAGE invocation executes (leg B consumer) (exit=0)
PASS: guest-visible fixture lands after make run recipe (leg A)
PASS: agreement test is falsifiable (a diverged tier stage differs from the make leg) (mk=b'\x7fELF-agreement-probe' tier_perturbed=True)
Tests run: 8, failed: 0
EVIDENCE RIG TEST PASSED
```

**Stated plainly, as the card asks:** if the tier's line were broken, the two *executions* would not both
fail — leg B runs a string hard-coded in the test, not `t3_tier.sh` itself. What catches the broken tier is
the test's **live-read assertion**, and I measured that it does (item 2: rc=1, one failure, named). So the
answer to "would both legs still pass if the tier's line were broken" is **no** — but the mechanism is the
assertion, not leg B's execution. That is a narrower guarantee than "leg B executes the tier": it catches a
line that stops carrying the tokens, not one that keeps the tokens and behaves differently.

## 2, 3 — both directions of drift, run

Tier side broken in place (`ROOT="$ROOT" sh -c "$T3_STAGE"` → `sh -c "$T3_STAGE"`):
```
FAIL: t3_tier.sh still invokes T3_STAGE via ROOT=$ROOT sh -c (live read) ()
Tests run: 8, failed: 1
EVIDENCE RIG TEST FAILED        RC=1
```
Makefile side broken in place (`$$RUN_STAGE` → `$RUN_STAGE`, the original defect):
```
FAIL: Makefile run recipe still drives RUN_STAGE via ROOT=$(CURDIR) shell (live read) ()
Tests run: 8, failed: 1
EVIDENCE RIG TEST FAILED        RC=1
```
Both restorations re-ran clean (`git status` clean apart from the one untracked build artefact).

## 4, 5 — in the gate, stubbing disclosed

```
  PASS  tools/evidence_rig_test.py
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
RC=0
```
The test's own count is 8 tests (the suite's is 482 checks). The stub is `QEMU_CMD = @:` — quoted above —
and item 3 proves the recipe line is still read from the real `Makefile`, so the stub cannot hide a
contract drift.

## What this GREEN claims and does not

- Claims: at `9866b7de` the agreement test exercises two genuinely different consumers, its drift
  assertions fire on **both** files (measured, not read), and it is green inside `make host_tests`.
- Does not claim: that leg B executes `t3_tier.sh` — it replays the tier's line and is bound to the file by
  a token-level live-read assertion (caveat above). If a future card wants execution-level binding, that is
  the remaining gap, and it is smaller than the one this card was minted to close.

## Reproduce

```
git worktree add /tmp/w118v3 --detach 9866b7de && cd /tmp/w118v3
python3 tools/evidence_rig_test.py                       # 8 tests, 0 failed
sed -i '182s/.*/  sh -c "$T3_STAGE" || exit 2/' tools/t3_tier.sh && python3 tools/evidence_rig_test.py   # rc=1
git checkout -- tools/t3_tier.sh
sed -i '2478s/\$\$RUN_STAGE/$RUN_STAGE/' Makefile && python3 tools/evidence_rig_test.py                  # rc=1
git checkout -- Makefile && make host_tests                                                             # 482/0
```