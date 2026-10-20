# GB-007-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `2ffa3be` (clean worktree, `git status` empty apart from this evidence dir).
Gate: exactly the DoD commands — `bash src/host/t4_parity.sh`, plus one injected leg-B divergence and a revert.

## What passes (measured, not asserted)

| log | command | result |
|---|---|---|
| `01_baseline.log` | `MUSL_GCC=~/toolchains/musl/musl-gcc bash src/host/t4_parity.sh` | `A=0 B=0 cases=3`, `t4_parity: PASS`, EXIT=0 |
| `02_injected_lie.log` | same + `HOST_CFLAGS='… -DT4V_INJECT_LIE'` (leg-B-only `write_stdout rc=2`) | `FAIL: stdout differs (legA_musl vs legB_host)`, `t4_parity: FAIL`, **EXIT=1** |
| `03_reverted.log` | injection reverted (`git checkout -- src/host/t4_compat_probe.c`) | `t4_parity: PASS`, EXIT=0 |

The lie detector is real: one wrong `rc` in leg B flips the harness to exit 1 and the differing row
(`write_stdout rc=9` vs `rc=2`) is printed in both legs' dumps.

## What fails — the gate reports a verdict it did not measure

`t4_parity.sh:111` runs the differential only when **both** legs built; if `musl-gcc` is absent it sets
`A=skip`, prints `SKIPPED (missing toolchain)`, leaves `fail=0`, and falls through to the PASS line
(`t4_parity.sh:138-140`). The Makefile target CI uses (`Makefile:3299-3300`) is a bare
`@bash src/host/t4_parity.sh` with no musl requirement, so this is the suite's real behaviour on any
farm VM that has not been provisioned:

```
$ env -u MUSL_GCC make t4_compat_parity
SKIP leg A: musl-gcc not found (install musl musl-tools)
t4 host differential: SKIPPED (missing toolchain: A=skip B=/tmp/…/t4_probe.host)
t4_parity: PASS (host differential locked; device leg gated)
EXIT=0
```

(`05_make_target_degraded.log`; `04_degraded_no_musl.log` is the same path invoked directly.)

So the harness prints **"host differential locked" and exits 0 having compared zero rows**, and it is a
prerequisite of `host_tests`. That is a skipped gate presented as a verdict — the DoD's rule 1
("a SKIP … is not a verdict", also GB-066's "a missing/absent probe fails the tier rather than
skipping it") and §4.2's honesty law. A gate that cannot fail when its reference leg is absent is not
a lie detector; it is a lock with the key left in it.

Secondary, same file: the row count printed is `cases=3` (`capture legA`, `capture legB`, `cmp_out`) while
the probe emits 64 rows — the number the DoD asks to be printed is the harness's own increment, not a
row count. And `cmp_out` uses `diff -q`, so the FAIL names the *legs*, not the row (`write_stdout` is
identifiable only by eye in the two dumps).

## Fix shape (small — this is one patch from GREEN)

1. `SKIP leg A`/`SKIP leg B` ⇒ `echo "t4_parity: FAIL (leg skipped — no coverage)" >&2; exit 1`
   (or make the Makefile target depend on `musl-tools`, but the script must not pass either way).
2. Print the real row count (`grep -c` of the probe's output lines) instead of `cases=`.
3. `diff -u` the two outputs so the failing row is named, not eyeballed.

## Scope statement

The device leg is `SKIP` by design (GB-066 owns it) — this verdict says **nothing** about
parity in a HobbyOS guest. Everything above is the host leg.
