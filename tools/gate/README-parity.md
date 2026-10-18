# Two-arch parity CI (GB-044)

`tools/gate/parity_ci.sh` enforces the two-arch parity rule (gaming.md §2.3)
on every merge: it runs the per-arch check on **ARCH=arm** and **ARCH=intel**
and requires **both** to be green. It also enforces the ticket's DoD
**"run window < best"**: the arches run concurrently to bound wall-clock, and
a `PARITY_BUDGET_S` baseline (the best-known window) fails the CI when the
measured window exceeds it.

## What it verifies

- Both arches green -> **PASS**.
- Either arch red (arm or intel) -> **FAIL**.
- Measured window over the budget -> **FAIL** (a parity CI that drifts past
  the best-known baseline must fail, not silently slow).
- Concurrency: with `PARITY_SEQUENTIAL=0` (default) the two arches run in
  parallel, so the wall-clock window is at most the slower arch, not the sum.

## Usage

```sh
./tools/gate/parity_ci.sh                 # both arches, default per-arch check
make parity_ci
make test_parity_ci                       # self-test (fast; proves the verdict)
```

Per-arch runner: defaults to the merge gate `tools/gate/merge_gate.sh` when
present (lands with GB-005). Until then (or to run a specific check), supply
commands:

```sh
PARITY_ARM_CMD="bash tools/t3_tier.sh police <clean-arm-log> arm" \
PARITY_INTEL_CMD="bash tools/t3_tier.sh police <clean-intel-log> intel" \
./tools/gate/parity_ci.sh
```

`GATE_T1/T2/T3` and `GATE_CI` are forwarded to the merge gate when used, so a
farm/CI sets `GATE_CI=1` for the full T1-T3+T5 wave on both arches.

## Verdict semantics

A tier's verdict is PASS or FAIL; nothing silently passes. If no per-arch
command and no merge gate are available, the CI returns exit 2 ("refusing
rather than silently passing") rather than a false green.

## Self-test (`parity_ci_test.sh`)

Proves the verdict + budget with stub per-arch commands and two real
`tools/t3_tier.sh police` logs (no QEMU needed): two green -> PASS; arm red /
intel red -> FAIL; window over budget -> FAIL; concurrent window <= sequential;
clean-log wiring -> PASS, FAIL-injected intel -> parity FAIL.
