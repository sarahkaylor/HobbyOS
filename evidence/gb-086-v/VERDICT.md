# GB-086-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-24.
Revision: PR head **`ad51ac5`** (`ticket/086-polltst`), clean detached worktree; ARM tier run here at
`T3_SMP=1` (8 samples) and at the default 8 CPUs, plus the host differential.

Two of the four DoD criteria do not hold, and one of them cannot even be attempted on the shipped tree.

## 1. `elapsed_ms=<digits>` at both CPU counts — PASS

```
1 CPU :  POLLTST elapsed_ms=9491 | 857 | 2679 | 4642 | 2679 | 662 | 676 | 653   (8 samples)
8 CPUs:  POLLTST elapsed_ms=156
```

One atomic line each, digits intact — the emitter half of the PR works.

## 2. Select-timeout check — FAIL (flaky at 1 CPU, and wildly imprecise)

```
sample 1 (1 CPU):  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL
                   POLLTST elapsed_ms=9491
                   POLLTST FAILED: 1 check(s)
samples 2-8 (1 CPU): PASS, elapsed_ms = 857, 2679, 4642, 2679, 662, 676, 653
8 CPUs:              PASS, elapsed_ms = 156
```

The check asks for a 100 ms timeout and bounds it at `< 5000`; at 1 CPU the same tree produced
**653 → 9491 ms** across 8 runs, and one run exceeded the bound outright. At 8 CPUs the same code
produces 156 ms. The bound is unchanged in the source (`elapsed >= 85 && elapsed < 5000`,
`src/user/poll_test.c:186-187`), so this is not a stale expectation — it is timing behaviour that depends
on CPU count and is not reliable at 1 CPU.

Disclosure: sample 1's console log was overwritten by the 8-CPU run before I archived it (my error).
Samples 2–8 are archived in `evidence/gb-086-v/`. Even leaving sample 1 aside, the archived spread
(4642 ms against a 5000 ms bound) leaves the check within 8 % of failing on a healthy tree — not a gate.

## 3. Negative control (`POLLTSTBRK.BIN`) — UNMEASURABLE: never built or staged on ARM

```
$ mdir -i disk.img ::/ | grep -i POLLTST
POLLTST  BIN     50400      # the healthy test
POLLTSTB BIN         0      # the "control": 8.3-truncated name, ZERO bytes

$ ls obj/arm/POLLTSTBRK.BIN
ls: cannot access 'obj/arm/POLLTSTBRK.BIN': No such file or directory

# staged a WAVE.CFG naming it and booted:
[WAVE-MANIFEST] POLLTSTBRK.BIN
[WAVE-TRY] POLLTSTBRK.BIN
[ARRIVED] POLLTSTBRK.BIN rc<0        ← never loaded, so it can never fail the check
```

`POLLTSTBRK_BIN` is defined in the Makefile (`:1018-1024`) but is **not** a prerequisite of `disk.img`
(`:2169`) and has no mcopy rule, so no ARM build of it exists. The 0-byte `POLLTSTB.BIN` entry is the
truncated-name artifact of an earlier staging attempt. DoD criterion 3 — "the broken-select control
FAILS at both counts" — cannot be satisfied because the control never runs.

## 4. Host differential — PASS

```
$ make t4_compat_parity      → A=0 B=0 cases=3, t4_parity: PASS
legA: select_timeout_rc rc=1  select_timeout_fdset rc=1
legB: select_timeout_rc rc=1  select_timeout_fdset rc=1
legs byte-identical (84 lines)
```

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | `elapsed_ms=<digits>` both counts | **PASS** |
| 2 | select-timeout PASS at both counts (or bound changed with distribution quoted) | **FAIL** — 1 of 8 1-CPU samples `FAIL` at 9491 ms; archived spread 653–4642 ms for a 100 ms request; bound unchanged |
| 3 | broken control FAILs at both counts | **UNMEASURABLE** — `POLLTSTBRK.BIN` not built/staged on ARM (0-byte `POLLTSTB.BIN`), `[ARRIVED] rc<0` |
| 4 | `t4_compat_parity` PASS, `select_timeout_*` identical | **PASS** |

## Also seen (not this card's ground)

Both tier runs end `SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST` → `T3_arm EXIT=1`. That is
GB-058's ARM defect; it is not counted against GB-086 here.

## What closes it

1. Explain/fix the 1-CPU select-timeout spread (0.65–9.5 s for a 100 ms request; 156 ms at 8 CPUs) —
   timer arming per CPU, tick quantisation, or the waiter's CPU being descheduled. The card forbids
   widening the bound merely to pass.
2. Make the control real: add `POLLTSTBRK_BIN` to the `disk.img` prerequisites with an mcopy, and give it
   an 8.3-legal name (`POLLBRK.BIN`) so `[ARRIVED]` is `rc>0` and the check can be shown to fail.
3. Re-run both counts with the control.
