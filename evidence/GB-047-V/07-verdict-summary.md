# GB-047-V — scanner verdict, revision de0cc46

Worktree `hermes-9642df51`, branch `verify-gb047v` at **de0cc46** (= `origin/main`, the sha the card
names as the merged pack), swarm-farm-2.

## Criterion 1 — what the scanner finds on the real tree

```
$ python3 tools/t5/evidence_pack.py --root . --out /tmp/PACK-REAL.md
[pack] wrote /tmp/PACK-REAL.md + S0-S1-EVIDENCE-MANIFEST.json (12 artifacts)      exit=0
```

12 artifact rows: `tools/evidence_rig.py`, `tools/perf_game_cycle.py`, 5 replays under
`tools/t5/replays/`, `perf-baseline-REPORT.md`, `perf-global-REPORT.md`, 2 `game evidence` rows.
Nothing is reported missing — the pack has no "missing" concept at all.

**"0 findings" is not distinguishable from "looked nowhere".** Pointed at an empty directory:

```
$ python3 tools/t5/evidence_pack.py --root /tmp/emptypack --out /tmp/PACK-EMPTY.md
[pack] wrote /tmp/PACK-EMPTY.md + S0-S1-EVIDENCE-MANIFEST.json (0 artifacts)      exit=0
```

Same success status, same document shape, headers only.

## Criterion 2 — it does NOT fail on a missing artifact (the failing check)

```
$ python3 tools/t5/evidence_pack.py --root /tmp/emptypack --out /tmp/PACK-EMPTY.md
[pack] wrote … (0 artifacts)      exit=0

$ mkdir -p /tmp/partpack/evidence; cp perf-baseline-REPORT.md /tmp/partpack/
$ python3 tools/t5/evidence_pack.py --root /tmp/partpack --out /tmp/PACK-PART.md
[pack] wrote … (1 artifacts)      exit=0
```

An empty tree and a tree missing the rig, every replay and every other marker both exit **0**. The
card names this exact outcome as RED: *"If it exits 0 saying 'pack complete' over an empty tree, this
card is RED."*

The suite has no absence clause to fail. `tools/t5/evidence_pack_test.py` is 33 lines and its four
checks are all positive-discovery:

```
PASS: sha256 deterministic
PASS: find_reports picks up EVIDENCE-REPORT
PASS: resolve_marker finds rig
PASS: resolve_marker globs replay scripts
Tests: ALL PASSED            exit=0
```

Delete every artifact and this test still passes — it builds its own temp tree and never asks whether
a *missing* one is detected.

## Criterion 3 — counts vs the tree

```
$ git ls-files evidence/ | wc -l
89
scanner 'game evidence' rows ........ 2
scanner total artifact rows ......... 12
```

The scanner indexes 2 files from an `evidence/` tree that tracks 89. That gap is explicable (the
pack globs report-shaped artifacts, not the whole tree; the rest are force-added wave logs), but the
pack does not *state* it — it presents "12 artifacts" as the index, with no denominator and no
"missing" list, which is why the number cannot be audited from the document itself.

## Criterion 4 — discovery still covers the removed lines (passes)

```
$ python3 tools/t5/run_t5_tests.py
  PASS  tools/evidence_rig_test.py
  PASS  tools/t5/evidence_pack_test.py
  …
[host-tests] done: 20/20 passed          exit=0
```

Both named tests are discovered and pass, so the merge resolution did not silently drop a test.

## Verdict

**RED.** Criterion 2 fails: the instrument cannot tell a complete pack from an incomplete one — it
exits 0 with an empty inventory and calls that a written pack. Criterion 1 fails with it, because a
count of 0 is indistinguishable from a scan that found nothing to look for. Criteria 3-4 hold and are
quoted above so the RED is scoped to the scanner, not to the evidence the pack describes.

The fix is small and specific: a required-artifact list the scanner compares against, a `missing`
section in the document, and a non-zero exit when it is non-empty — plus the absence case in
`evidence_pack_test.py` so the suite can fail.
