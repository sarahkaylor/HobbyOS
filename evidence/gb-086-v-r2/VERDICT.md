# GB-086-V (round 2) — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-24.
Revision: **`36ab259`** (`GB-089/GB-087: null-entry control gate + kernel_exit fix (#114)`), clean worktree, ARM and intel tiers on this VM.

Clause 3 is now real and it works. Clause 2 — the cell the card says was never measured — **fails at 1 CPU**,
and the failure is not the old kernel fault: the guest now reaches the check, runs it, and the guest's own
measurement misses the bound.

## Clause 1 — `elapsed_ms=<digits>` at both counts: PASS

```
1 CPU (full wave):  POLLTST elapsed_ms=9326
8 CPUs (full wave): POLLTST elapsed_ms=2207
```

Both emitted atomically. The bound is untouched: `36ab259^ → 36ab259` produces an **empty diff** for
`src/user/poll_test.c`, and the row is byte-identical at the round-1 head `ad51ac5` and at `36ab259`:

```
  check("timeout took >= 85ms and < 5000ms (elapsed seen)",
        elapsed >= 85 && elapsed < 5000);      /* poll_test.c:186-187 */
```

Nobody widened it to pass.

## Clause 2 — the check passes at both counts: **FAIL at 1 CPU**

```
  POLLTST empty pipe: select(timeout 100ms) == 0: PASS
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL
  POLLTST elapsed_ms=9326
```

At `T3_SMP=1` on the fixed tree the check runs (no `kfatal`, no GB-087 null-entry) and reports 9326 ms for a
100 ms timeout — 93× the ask, nearly twice the 5000 ms bound. At 8 CPUs the same tree, same wave: PASS,
`elapsed_ms=2207`. The card says it plainly: *"If it still FAILs now, that is a real finding and the card
stays open."* It still FAILs.

### New datum that narrows the cause

The falsification leg put the **healthy** `POLLTST.BIN` through the same 1-CPU boot as the control (single-program
manifest) and got:

```
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): PASS
  POLLTST elapsed_ms=108
```

108 ms — i.e. at 1 CPU with nothing else in the wave, the select honors its 100 ms ask. The 9326 ms only
appears in the **full wave**. So this is not a flat "1 CPU is 10× slower" factor: the timeout is distorted by
what else is resident in the wave (a long non-preemptible kernel section, tick starvation, or the waiter being
descheduled for ~9 s of other work). That is the lead the implementation needs; it is not something I can
settle without instrumenting the scheduler, which is the implementation ticket's job, not the gate's.

## Clause 3 — a broken `select` still FAILs: PASS, four cells

| arch | T3_SMP | result |
|---|---|---|
| arm | 1 | `halted=1 loaded=1 timeout_FAIL=1 elapsed0=1 failed=1 kfatal=0` → `t3_pollbrk: PASS` |
| arm | 8 | `halted=1 loaded=1 timeout_FAIL=1 elapsed0=1 failed=1 kfatal=0` → `t3_pollbrk: PASS` |
| intel | 1 | `halted=1 loaded=1 timeout_FAIL=1 elapsed0=1 failed=1 kfatal=0` → `t3_pollbrk: PASS` |
| intel | 8 | `halted=1 loaded=1 timeout_FAIL=1 elapsed0=1 failed=1 kfatal=0` → `t3_pollbrk: PASS` |

Control row, exactly as required:

```
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL
  POLLTST elapsed_ms=0
```

This is a real improvement over round 1, where the control never loaded (`[ARRIVED] POLLTSTBRK.BIN rc<0`).

## Falsification the card demands (gate must be able to fail): PASS

Healthy `POLLTST.BIN` copied over `obj/arm/POLLBRK.BIN`, `T3_SMP=1`, same gate:

```
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): PASS
  POLLTST elapsed_ms=108
t3_pollbrk: FAIL — control did NOT fail the timeout check (gate paintable green!)
t3_pollbrk: FAIL — no elapsed_ms=0 row
t3_pollbrk: FAIL — POLLTST did not report FAILED
t3_pollbrk: FAIL
gate rc=1
```

So the gate distinguishes broken from healthy. It is not a rubber stamp. (`obj/arm/POLLBRK.BIN` restored from
backup afterwards.)

## DoD table

| # | clause | result |
|---|---|---|
| 1 | `elapsed_ms=<digits>` both counts; bound unchanged | **PASS** — 9326 / 2207; `poll_test.c` untouched by the revision |
| 2 | select-timeout PASS at both counts | **FAIL** — 1 CPU, `elapsed_ms=9326`, row `FAIL` |
| 3 | broken control FAILs, four cells | **PASS** — 4/4, `elapsed0=1 kfatal=0` |
| 3b | gate falsification (healthy control) | **PASS** — gate exits 1, "gate paintable green!" |

## Not counted against this card

The full-wave 8-CPU serial interleaves console output mid-line (`POLLTST pipe write end with space: select(write, 0) == 1: PASS` merged with `[LKSTEST]`, `open second fixture  POLLTST`) — that is GB-082's glued-line defect, and the `SHELLTEST WATCHDOG: protocol stalled 25 s` that ends both full waves is GB-058. Neither is clause-2 evidence.

## What closes it

1. Find why a 100 ms select takes 9.3 s when the full wave is resident at 1 CPU while the same call takes
   108 ms in an otherwise-idle 1-CPU boot — scheduler wake latency, a long non-preemptible section, or tick
   accounting. Quote the instrumented number, not a theory.
2. Re-run the same grid: full wave ARM 1 CPU and 8 CPUs, four `t3_pollbrk` cells, falsification leg.
