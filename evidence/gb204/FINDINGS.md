**FINDINGS — GB-204: 1.2 atomic claim and 1.3 dropped counter are implemented; the two-CPU T3 proof is NOT delivered.** Branch `gb204-panic` (based on PR #254 head `ef09aa9a`). This is a debug-card finding, not a RED/GREEN verdict.

## Delivered

**1.2 atomic claim.** `src/kernel/arch/x64/uart.c`:

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

`src/kernel/arch/x64/trap.c` uses it: `if (!panic_claim()) { for (;;) __asm__ volatile("hlt"); }` — the loser counts the drop, writes nothing, and stops its CPU; `panic_leave()` still runs after the dump.

**1.3 dropped counter, counted before the cap.** `panic_sink_violation()` now increments `panic_sink_dropped` first, reports `[PANIC-SINK] suppressed=N` on COM2 for every violation (uncapped; nothing else writes COM2), and only then applies the COM1 record cap of 3. Measured on intel, `MODE=test`, default 8-CPU wave, COM2 captured with `-serial file:`:

```
[PANIC-SINK] suppressed=73599
[PANIC-SINK] suppressed=73600
...
[PANIC-SINK] suppressed=73613
com2_lines=73809
```

The count is far past the cap (3): drops past the cap still move. The COM1 `[PANIC-SINK-VIOLATION]` records are still emitted (first three) and are still shredded by their own byte-wise writer — pre-existing GB-097 behaviour, not this card.

**Regressions.** Allowlist updated deliberately (`uart.c` 15 → 20 references; the self-test still proves new/deleted callers fail). `make host_tests` → 482 checks / 0 failed / `TEST EXIT: 0`; `make ARCH=intel hobbyos.elf` rc=0; `make ARCH=arm hobbyos.elf` rc=0.

## Not delivered — the blocker

The DoD's clause 1 wants **a two-CPU injected run** showing one record, no shredding, and clause 3 wants the pre-fix two-CPU interleave to contrast. No such vehicle exists in the tree. I attempted one: a `KERNEL_MODE_TEST` syscall (`0xFE`) that faults at `0xdead0000`, plus a user program calling it, staged twice in a two-member `::WAVE.CFG`. The programs loaded (`[ARRIVED] GB204A.BIN / GB204B.BIN rc>0`) but never reached `main`: both sat at `[IDLESTUCK] ... rip=0x10000001d7` in libc startup, so no fault reached the sink. I reverted the vehicle (syscall branch, user program, Makefile rule) rather than ship a dead test path.

Consequence: **the claim is implemented but unproven at T3**, and there is no measured pre-fix interleave line. A next pass needs a working two-CPU injection — a kernel-side IPI hook with a C vector stub is the shortest path (the production code is ready for it); a user fault vehicle needs the libc-startup park understood first.

## Files

- `src/kernel/arch/x64/uart.c`, `src/kernel/arch/x64/trap.c`, `tools/gate/panic_sink_allowlist_test.py`
- Evidence on the branch: `evidence/gb204/` (build, wave/COM2 serial, host_tests, links, the failed vehicle log)

Lane: **In Review** for the Coordinator (findings, not a verdict).
