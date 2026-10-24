# Flaky-test triage workflow + quarantine lane (GB-050)

**Owner** wgames (swarm-farm-3). Purpose: a repeatable way to catch a flaky or
timed-out test, reproduce it, quarantine it out of `host_tests`, fix it, and
re-promote it — so a single unstable test cannot hold the gate hostage without
a documented decision.

## The lane

A test joins the **quarantine lane** (recorded in `tools/t5/quarantine.json`)
only through `tools/t5/flaky_triage.py quarantine <name> <reason>`. A quarantined
test is excluded from `host_tests` until it is either fixed and
`flaky_triage.py release <name>`d, or permanently removed. Every entry records
`reason` + `since` (date) so the lane is auditable, never a silent hop.

## Workflow

1. **Detect** — a full gate (`make host_tests` or a T3 run) shows a FAIL /
   ERROR / timeout / SIGKILL/SEGV/ABRT line.
   `python3 tools/t5/flaky_triage.py scan <run.log>` names the failing tests and
   buckets the marker kinds (FAIL/ERROR/TIMEOUT/CRASH/ASSERT).
2. **Reproduce** — rerun the suspect test in isolation. *This is the rule that
   separates flaky from broken:* run it ≥3 times.
   - fails **every** time        → **BROKEN**: fix (or quarantine with a fix ticket).
   - fails some, passes some     → **FLAKY**: the genuine quarantine candidate.
   - a wall-clock timeout        → **TIMEOUT**: quarantine; re-promote only with a
     determinism/budget fix.
3. **Quarantine** — `flaky_triage.py quarantine <test> "flaky: 2/5 runs, repro log <url>"`.
   This is the only sanctioned way to drop it from the gate.
4. **Fix + release** — land the fix, then `flaky_triage.py release <test>`.
5. **Audit** — `flaky_triage.py ls` lists the lane; the dates make stale
   quarantines visible (anything >2 sprints gets reviewed at the M4 acceptance).

## Rules that keep it honest

- **No silent quarantine**: quarantine always requires a `reason`; the reason
  names the evidence (run n/m + log).
- **Evidence before quarantine is law** (matches the swarm's §4.2/§5.3):
  classify with reproduced runs, never a single red line.
- **The gate is the workflow's owner**: a test cannot leave the gate just by
  editing host_tests; it must go through `quarantine`/`release` so the lane has
  a paper trail.
- **T1-testable core**: `FlakyClassifier` (BROKEN vs FLAKY vs PASSING) and
  `classify_line` are covered by `tools/t5/flaky_triage_test.py` in `host_tests`.
