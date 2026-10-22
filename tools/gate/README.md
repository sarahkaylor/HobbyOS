# Merge gate (GB-005) — `tools/gate/`

The branch/PR merge gate: one command that runs the HobbyOS test tiers and
turns their output into a single verdict a PR must pass before Merge Gate.

## Why it exists

Every worker PR in this program collides on the shared `host_tests:` line;
GB-064 removed the *structural* half of that (self-registering python host
tests), and this is the other half — the gate that actually judges a branch.
It inherits the *real* T3 green criteria from `tools/t3_tier.sh` (no FATAL
kernel-mode fault, a console above the completeness floor, and a `System halt`
completion marker) rather than trusting a bare exit status.

## The tiers (verdict = PASS / FAIL / SKIP; never a silent pass)

| tier | command | notes |
|---|---|---|
| T1 | `make host_tests` | host golden suite (includes t4 parity, elf/linux-abi, gfx, evdev, NES, self-registering python tests GB-064) |
| T2 | `make unit_tests_{arm,intel}` | in-guest kernel unit waves (QEMU), both arches |
| T3 | `tools/t3_tier.sh {arm,intel}` | the policed integration gate, both arches |
| STYLE | `make check-format` + `tools/test_cstyle.py` | reported; default off (pre-existing main violations) |

Heavy QEMU tiers (T2/T3) default **off** so the gate is cheap to run on any
host; the farm/CI turns them on. A disabled tier is reported SKIP, never a
silent PASS, and the vet any given run isn't passing tiers it didn't run.

## Usage

```sh
./tools/gate/merge_gate.sh            # T1 locally (fast, real)
GATE_CI=1 ./tools/gate/merge_gate.sh  # full gate: T1 + T2(arm,intel) + T3(arm,intel) + style
GATE_T3=1 ./tools/gate/merge_gate.sh  # T1 + T3 integration waves
make merge_gate                        # same as the first
make merge_gate_ci                     # full gate
```

T3 verdict over an already-captured console log (no QEMU):

```sh
./tools/gate/merge_gate.sh police <arch> <log>
```

Every tier's real command and exit code land in `obj/gate/`, so a verdict is
reproducible from the log alone.

## DoD self-test — `merge_gate_test.sh`

Proves **"a dummy branch fails -> passes the gate"**:

- Part A (fast, no QEMU): a clean captured log is GREEN; a byte-identical log
  with one injected `FAIL` line (a dummy branch outputting a failure) is RED;
  an injected `FATAL kernel-mode fault` is RED too.
- Part B (opt-in `TEST_RUN_T1=1`): the real T1 gate on the working tree must be
  GREEN.

```sh
bash tools/gate/merge_gate_test.sh            # Part A (fast)
TEST_RUN_T1=1 bash tools/gate/merge_gate_test.sh  # + real T1 gate
make test_merge_gate
```

## Env

`GATE_T1 GATE_T2 GATE_T3 GATE_STYLE` (defaults `1 0 0 0`); `GATE_CI=1` enables
the full set; `GATE_FAIL_TAIL` (default 25) is how many lines of a failing
tier's log to print.
