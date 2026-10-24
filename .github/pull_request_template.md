# Pull Request — HobbyOS (branch-per-ticket)
<!-- Replace the bracketed text. One PR == one ticket; the branch is
     `ticket/<NN>-<slug>` cut from `origin/main`. -->

## Ticket
- **GB-XXX** — [ticket title]: [one-line summary]
- Board: https://192.168.0.234:3000/sarah/gaming-program/issues/XXX

## What & why
[What changed, why, and the ticket's Definition of Done this lands.]

## Test tiers (the merge gate, `tools/gate/merge_gate.sh`)
Required before Merge Gate — paste the real output of each tier you ran:
- [ ] T1 — `make host_tests` : [PASS / FAIL / SKIP — paste tail]
- [ ] T2 — `make unit_tests_{arm,intel}` : [if on the gate; QEMU]
- [ ] T3 — `tools/t3_tier.sh arm` (and `intel`) : [if on the gate; QEMU]
- [ ] parity : t4_parity / two-arch (S0: must stay green on **both** ARCH=arm and ARCH=intel)
- [ ] evidence : real command + output attached (serial / screendump / gate run) — **evidence before close** (§4.2, §5.3)

## Verify-by-execution checklist (do not check a box from a diff)
- [ ] `grep -rc '<<<<<<<' <changed files>` = 0
- [ ] `make -n host_tests` parses clean
- [ ] every new test file is staged (a Makefile rule with its test left out breaks `main`)
- [ ] conflict in the shared `host_tests:` line resolved as a **union** (keep both sides)

## Branch / merge note
Do not resolve shared-line conflicts by replacing the other side; keep both
(GB-005 union rule). The merge order for colliding games PRs is decided by the
coordinator — say if a conflict blocks `make`, not that your code is at fault.
