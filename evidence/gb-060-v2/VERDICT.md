# GB-060-V2 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-060-V2 (#217) · verifies GB-060 (#62) · ship = branch `ticket/060-lockcnt` head **`b098202`**
(+95/−0, 5 files: `src/host/locksteal_test.c`, `src/include/lock.h`, `src/kernel/arch/{arm,x64}/locks.c`,
`src/kernel/process.c`)
Gate tree `832342f` = `origin/main` `edd60fd` × `b098202` (`ort`, clean)
Run window 2026-10-27T03:00Z–03:5xZ · local only — no CI consulted.

## VERDICT: GREEN

Both holder shapes recover, each quoted separately from a real T3 intel run, the holder is named, the
boundary holds, and the two rules share one symbol.

| # | Clause | Result |
|---|---|---|
| 1 | shape A — stopped holder: reaper fires, holder named | **PASS** — `[STOLEN] … from halted cpu=6` |
| 2 | shape B — wedged holder (alive, no timer IRQs) | **PASS** — `[STALLBREAK] … owner_cpu=7 stale_ms=2052` |
| 3 | boundary exact + a deliberate break fails the suite | **PASS** — 31/31; rule break → 1 FAIL (caveat below) |
| 4 | one symbol, no drift | **PASS** — `LOSTWAKE_DEAD_OWNER_MS` (process.h:588) in both arches |
| 5 | T3 intel reaches `System halt`, no `[HOLDER]` stall, FAIL count not worse | **PASS with a named caveat** |

### 1 and 2 — the two shapes, from the same in-guest run

    [STOLEN] cpu=3 lock=0x0000000075501ee0 from halted cpu=6 pc=0x000000007003b82e
    [STALLBREAK] cpu=5 lock=0x0000000070200250 owner_cpu=7 stale_ms=2052 pc=0x000000007000149c
    [LOCKCNT] STALLBREAK=1 STOLEN=1 LOCKFOREVER=1 UNOWNED-FREED=0

Shape A is a halted holder (`[STOLEN]`, halt-keyed rule); shape B is a *live* holder whose heartbeat is
older than the grace period (`stale_ms=2052 > 2000`), freed by `lock_stallbreak_ok()` — a different rule,
not the halt rule. The counters prove each fired exactly once in this wave, so neither is inferred from the
other.

The deterministic host suite shows the same two rules separately:

    PASS: halted owner (cpu1): freed
    PASS: any waiter may free a halted owner's lock
    PASS: LIVE holder with a fresh heartbeat: never broken, however long the waiter spins
    PASS: one ms inside the grace period: still protected
    PASS: at the grace period: broken (>=, the same rule the lost-owner reaper uses)
    PASS: long stale: still broken
    PASS: halted owner: broken immediately, despite a fresh heartbeat
    locksteal_test_host: 31 checks, 0 failed                                    rc=0

(31, not the 27 the card expects — `b098202` adds the `[LOCKCNT]` block with four more checks.)

### 3 — the boundary, and the break that works

Four boundary cases are asserted (quoted above: inside → protected, at → broken, fresh → never, halt →
immediate). Breaking the **rule** — `>=` → `>` at `src/kernel/arch/x64/locks.c:113` — fails it:

      FAIL: at the grace period: broken (>=, the same rule the lost-owner reaper uses)
    locksteal_test_host: 31 checks, 1 failed                                    rc=1

restored → 31/0.

**Two caveats I am naming rather than smoothing over:**

1. The card asks to "shift it by a few ms". I did — `LOSTWAKE_DEAD_OWNER_MS 2000 → 1995`, forced rebuild —
   and the suite still passed 31/0, because the test builds its expectations from the same constant
   (`host_now_ms = 100000 + LOSTWAKE_DEAD_OWNER_MS ± 1`). The fixture is self-referential: it can catch a
   wrong *comparison*, not a moved *threshold*. If the threshold itself needs a pinned test, it has to be
   written against a literal.
2. `$(LOCKSTEAL_TEST)` (`Makefile:2857`) lists `locksteal_test.c`, `arch/x64/locks.c` and `include/lock.h` —
   **not** `include/process.h`. Editing the constant does not rebuild the test ("make: 'locksteal_test_host'
   is up to date" with `1995` in the header). Harmless today, misleading later.

### 4 — one symbol

    src/include/process.h:588:#define LOSTWAKE_DEAD_OWNER_MS 2000
    src/kernel/arch/x64/locks.c:113:  return (now_ms - hb) >= LOSTWAKE_DEAD_OWNER_MS;
    src/kernel/arch/arm/locks.c:71:   return (now_ms - hb) >= LOSTWAKE_DEAD_OWNER_MS;

Both stall-break rules and the lost-owner reaper read the same `#define`; no second literal.

### 5 — T3 intel, with a baseline beside it

    patched (832342f): 11854 console lines, 91 FAIL lines, 6 user memory fault records across 6 processes,
                       85 programs exited, halted=1
                       [HOLDER] stall lines: 0            System halt: present
    baseline (main):   15851 console lines, 95 FAIL lines, 10 user memory fault records across 9 processes,
                       84 programs exited, halted=1

FAILs are lower, more programs exit, no `[HOLDER] … no cycle` stall remains, and the wave halts.

**The caveat:** the tier still exits 1 on the patched tree, on
`[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000000000007 … pid=57`. The
baseline run did not produce that line (0 occurrences in my one run; 13 FATAL lines of *other* shapes).
Before calling it a regression I checked the branch's older evidence: `vector=6 rip=0x0000000000000007`
appears 6 times in pre-GB-060 logs (`evidence/gb-057/t3intel_probe9_UD_at_rip7_pid31.log` and five others),
so it is the known "UD at rip 7" fault, not something this change introduced. That attribution rests on those
older logs plus one run each of mine — not on a repeat run, which the clock did not allow. **This GREEN
certifies the lock-recovery DoD, not wave health: the wave is red on `main` too.**

Not named by this card and not run: arm T3, T4, T5 — the DoD names T1 (the host suite, which is
`make host_tests`' `locksteal_test_host` target) and T3 intel only.