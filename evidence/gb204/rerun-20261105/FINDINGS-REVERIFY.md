# GB-204 — independent re-verification, 2026-11-05 (wgames)

Branch `gb204-panic` @ `19c6e8ad`, based on PR #254 head `ef09aa9a`. The DoD was re-scoped on
2026-11-05 (PLAN-016): clauses 1–3 here, the two-CPU injection proof is **GB-207's** (its PR #378
targets this branch). This file re-runs the card's own evidence; the original `FINDINGS.md` above
predates the re-scope and still calls the two-CPU proof a blocker — it is now GB-207's DoD.

## 1. Atomic claim — code present

`src/kernel/arch/x64/uart.c`:

```c
static volatile int panic_claim_word = 0;
int panic_claim(void) {
  if (__atomic_exchange_n(&panic_claim_word, 1, __ATOMIC_ACQ_REL) != 0) {
    int d = __atomic_add_fetch(&panic_sink_dropped, 1, __ATOMIC_RELAXED);
    panic_puts2("[PANIC-CLAIM] dropped=");
    print_int_raw2(d);
    panic_putc2('\n');
    return 0;
  }
  panic_depth++;
  return 1;
}
void panic_leave(void) {
  if (panic_depth > 0) panic_depth--;
  __atomic_store_n(&panic_claim_word, 0, __ATOMIC_RELEASE);
}
```

`src/kernel/arch/x64/trap.c:2910` — the fatal path is:

```c
if (!panic_claim()) {
  /* another CPU owns the fatal record: count the drop, write nothing, halt */
  for (;;) __asm__ volatile("hlt");
}
```

`grep -rn "panic_enter\|panic_claim" src/kernel` shows one fatal-path caller (`trap.c`), so no
uncounted path writes the record.

## 2. Dropped counter — counted before the cap, `suppressed=N` on COM2

`panic_sink_violation()` does `__atomic_fetch_add(&panic_sink_dropped, 1, ...)` **before**
`if (panic_sink_violations >= 3 || reporting) return;`, then prints
`[PANIC-SINK] suppressed=<n>` on COM2 for every violation (uncapped).

Re-run 2026-11-05, intel, `MODE=test`, default 8 CPUs (`QEMU_SMP` unset), COM2 captured with
`T3_QEMU_ARGS="-serial file:/home/sarah/.hermes/cache/scratch/com2_gb204.log"`:

```
count=158537
first=1
last=46230
```

(the count resets on each guest reboot — GB-146's FATALs reset the machine — so `last` is the final
boot's final value; excerpt in `com2_suppressed_excerpt.log`). Past the COM1 cap of 3 the count still
moves; the branch's original measurement (`suppressed=73599 … 73613`) is reproduced in kind, not
byte-for-byte (the wave's timing differs per boot).

`PANIC-CLAIM dropped=` count in this run: **0** — no CPU lost the claim during the wave's single-CPU
fatal; the loser path's proof is GB-207's injected two-CPU vehicle, as re-scoped.

## 3. Regression

```
console_shred.py --self-test      -> self-test: OK
panic_sink_allowlist_test.py      -> panic_sink_allowlist: PASS — every reference is accounted for
make host_tests (-j1)             -> Checks run: 482 / Checks failed: 0 / TEST EXIT: 0
make ARCH=intel hobbyos.elf       -> rc=0
make ARCH=arm hobbyos.elf         -> rc=0
```

`host_tests` must be run serially (`-j1`): `t4_compat_selftest` flips `src/host/compat.c` in place,
so with `-jN` it races `t4_compat_parity` and the parity leg can see the flipped source
(`open_nonexistent rc=999`). Serial run is green; that race is a suite bug, not this card's.

## 4. T3 intel — RED at this head, stated plainly

`tools/t3_tier.sh intel` ends `T3_intel EXIT=1` with `FPU_T FAILED: 2`, `THRD_T FAILED: 1`, and one
`FATAL kernel-mode fault ... vector=6` (GB-146's wave state). This card does not claim a green T3 —
its DoD is clauses 1–3 above; the FATAL/FPU/THRD failures are the wave's, not the panic-sink diff's
(the diff touches only `uart.c`/`trap.c` panic paths).
