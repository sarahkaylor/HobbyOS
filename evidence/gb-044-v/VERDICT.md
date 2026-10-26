# GB-044-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-044-V (#185) · verifies GB-044 (#44) · ship = on-main `tools/gate/parity_ci.sh`,
`parity_ci_test.sh`, `merge_gate.sh`
Run on `origin/main` @ `bba4e5e`, worktree `hermes-830a858d` (swarm-farm-1), 2026-10-26T06:23–06:31Z

Host-side gate only (no QEMU, no GitHub Actions — CI is ignored per the 2026-10-26 policy; nothing
below cites a check/status result).

## VERDICT: GREEN

| # | Clause | Result |
|---|---|---|
| 1 | `parity_ci_test.sh` PASS with every row behaving (both-green PASS, arm red FAIL, intel red FAIL by name, window over budget FAIL, parallel ≤ sequential, clean/fail-injected T3 wiring) | **PASS** — 7 rows, `PARITY_CI_TEST: PASS`, rc=0 |
| 2 | per-farm both-arch gate with the run window recorded | **PASS** — `PARITY: PASS — both arches green (window 59s; no budget set)`, arm exit 0 / intel exit 0 |
| — | the gate provably fails on an injected regression | **PASS** — one failing host check injected: both arches RED, refused **by name**, `PARITY: FAIL`, rc=1; reverted |

### Clause 1 — self-test, quoted

    --- 1. both arches green -> PARITY PASS ---
      PASS: two green arches -> PASS
    --- 2. arm red -> PARITY FAIL ---
      PASS: arm red -> FAIL
    --- 3. intel red -> PARITY FAIL ---
      PASS: intel red -> FAIL
    --- 4. window over budget -> PARITY FAIL ---
      PASS: window over budget (2s > 1s) -> FAIL
    --- 5. concurrent window <= sequential window (run window < best) ---
      parallel window=2s sequential window=4s
      PASS: parallel (2s) <= sequential (4s)
    --- 6. real t3-verdict wiring (no QEMU) ---
      PASS: clean log both arches -> PASS
      PASS: intel over FAIL-injected log -> PARITY FAIL

    PARITY_CI_TEST: PASS — parity verdict + run-window budget behave correctly
    rc=0

### Clause 2 — the real per-farm both-arch gate, quoted

    $ PARITY_ARM_CMD='ARCH=arm GATE_T1=1 GATE_T2=0 GATE_T3=0 bash tools/gate/merge_gate.sh' \
      PARITY_INTEL_CMD='ARCH=intel GATE_T1=1 GATE_T2=0 GATE_T3=0 bash tools/gate/merge_gate.sh' \
      bash tools/gate/parity_ci.sh
    === PARITY SUMMARY ===
      window: 59s   budget: 0s (0 = unbounded)
      arm   exit: 0     (GREEN)
      intel exit: 0   (GREEN)
      sequential: 0
    PARITY: PASS — both arches green (window 59s; no budget set)
    rc=0

The window is recorded per host as the DoD requires (`window: 59s`). With no `PARITY_BUDGET` set the
budget is deliberately unbounded; row 4 of the self-test proves the budget check works when one is set.

### The can-fail control on the real tree

One deliberately failing host check (`src/host/audio_stub_test.c`, injected then reverted — `git diff`
empty afterwards):

    === PARITY SUMMARY ===
      window: 45s   budget: 0s (0 = unbounded)
      arm   exit: 1     (RED)
      intel exit: 1     (RED)
      sequential: 0
      PARITY: arm is RED (log obj/parity/arm.log)
      PARITY: intel is RED (log obj/parity/intel.log)
    PARITY: FAIL (two-arch parity: both arches must be green and within budget)
    rc=1

A regressed tree is refused, and each arch is named — not a generic red.

### Scope note (what this gate does and does not cover)

It grades **host + unit parity plus the run window**, exactly as the card states; runtime T2/T3/
device parity is GB-012's, not this card's. `ARCH=arm`/`ARCH=intel` here select the build arch for the
gate's prerequisites while T1 (`make host_tests`) is host-side, so both legs exercise the same host
suite — that is the card's own command, not a substitution.