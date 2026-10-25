# GB-005 — debug-reserve findings: the merge gate's verdict line (DEEP verifier, swarm-farm-1)

Ticket GB-005 (#10, `debug`+`deep`) · worked 2026-10-25T08:46Z–09:05Z · worktree `hermes-ae371739`
Base `origin/main` `d55b004`. This is **debug work, not a gate**: no RED/GREEN verdict is claimed here.

## What was actually missing

GB-005's deliverable half landed long ago and is on `main`: `tools/gate/merge_gate.sh`,
`tools/gate/merge_gate_test.sh`, `.github/pull_request_template.md`, and the `merge_gate` /
`merge_gate_ci` / `test_merge_gate` targets. The card stayed open on **one outstanding, named
condition** from the Coordinator's PR #43 review (2026-10-18, re-stated 2026-10-18 21:09Z):

> the run ends `GATE: PASS (no required tier failed; skipped tiers are reported above)` while
> `T2_unit_arm/intel` and `T3_int_intel` all say `skip` — so `PASS` is printed on a run where neither
> arch was unit-tested and the guest was never booted… Either make it say what ran
> (`GATE: PASS (T1 only; T2/T3 skipped)`) or make `GATE_CI=1` the behaviour when the gate is used as
> a merge gate.

Measured on `d55b004` before any change: `grep -n "GATE: PASS" tools/gate/merge_gate.sh` →
`180:echo "GATE: PASS (no required tier failed; skipped tiers are reported above)"` — i.e. the
condition was never applied. The gate wiring landed; the honesty line did not.

## What I changed (two rules, both testable)

`tools/gate/merge_gate.sh`

1. **The verdict names what ran** — `GATE: PASS (ran: T1_host; skipped: T2_unit_arm …)`, and
   `skipped: none` when every tier ran. The old bare-PASS string is gone.
2. **A merge-gate run may not drop a core tier** — under `GATE_CI=1`, if `T1`/`T2`/`T3` is disabled
   the gate exits 1 *before doing any work*, naming the dropped tiers, instead of printing PASS over
   the absence:

       GATE: FAIL — GATE_CI=1 is a merge-gate run and may not drop a core tier: T1_host

`tools/gate/merge_gate_test.sh` — new **Part C** asserting both rules against the shipped script
(three checks: all-skip run names every skipped tier; no bare-PASS line; `GATE_CI=1` + dropped tier
→ exit 1 naming it).

## Evidence (all runs on this VM, this worktree)

**DoD sentence, end to end — "a dummy branch fails → passes the gate".** One deliberately failing
host check injected into `src/host/audio_stub_test.c`, gate run, then reverted, gate run again:

    ### RUN1 — dummy branch
      [GATE] T1_host: make host_tests
      [GATE] T1_host: FAIL (exit 2)
    GATE: FAIL — at least one required tier failed. Logs in obj/gate/.
    rc=1
    (obj/gate/T1_host.log: `FAIL: GB-005 DoD dummy: deliberately failing check (injected, reverted)`)

    ### RUN2 — same tree, dummy reverted
      [GATE] T1_host: PASS
    GATE: PASS (ran: T1_host; skipped: T2_unit_arm T2_unit_intel T3_int_arm T3_int_intel STYLE_check STYLE_test)
    rc=0

**Self-test, fixed tree** (`g1`, `g3`): Part A 3/3 (clean GREEN; FAIL-injected RED; FATAL-fault RED),
Part C 3/3, Part B with `TEST_RUN_T1=1` → `merge_gate.sh (T1) -> GREEN`, `MERGE_GATE_TEST: PASS`, rc=0.

**Negative control — the new tests can fail** (`g2`): with `origin/main`'s pre-fix `merge_gate.sh`
dropped back in, Part C fails all three checks and the suite prints `MERGE_GATE_TEST: FAIL`, rc=1.
Restored afterwards.

**T2 leg through the gate** (`g4`, `GATE_T2=1`): `T2_unit_intel: PASS` (`Tests failed: 0`,
`UNIT TESTS PASSED`), `T2_unit_arm: FAIL (exit 2)` — and that ARM red is **`main`'s defect, not the
gate's**:

    src/kernel/process.c:1882:27: error: use of undeclared identifier 'cpu_locals'
    src/kernel/process.c:1883:32: error: use of undeclared identifier 'cpu_locals'

The same x64-only-symbol-on-ARM defect that GB-099-V reported; it keeps every ARM tier dark and is
why a full `GATE_CI=1` cannot be green on `main` today.

## What I did NOT run, stated plainly

- **No full `GATE_CI=1`** (T1+T2+T3 both arches + style). It cannot pass on `main` for the `cpu_locals`
  reason above, and one T3 wave is tens of minutes; I ran the T2 leg instead to show the gate's second
  tier executes and reports a real failure. This is a scope statement, not a green claim.
- The change touches only `tools/gate/*.sh`, so its own merge needs T1 + the self-test; it does not
  alter any kernel or user code path.

## Note for the Coordinator

The card's DoD ("a dummy branch fails→passes the gate") is now demonstrated **at the gate level** with
the verdict line the review asked for, plus a self-test that fails on the pre-fix script. The remaining
blocker to a *green merge gate on main* is not GB-005: it is `main`'s ARM build (`cpu_locals`), which
has its own cards (GB-099 / the GB-085 stack).
