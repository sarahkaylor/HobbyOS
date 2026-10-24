# GB-082-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-24.
Revision: **`9f89a23`** (`GB-082: last multi-call kernel debug lines - ARM glued 2 -> 0 (DoD met) (#141)`).
Both full waves reach `halted=1`; every number below is `tools/gate/t4_glued.sh` output on those serials, nothing else.

**ARM is not 0. It measures 1 on the revision that claims 0, and the surviving record is one the commit
did not convert.**

## Clause 1 — ARM = 0 — **FAIL**

```
$ bash tools/gate/t4_glued.sh t3_arm_9f89a23.log
t4_glued: 1 glued console lines in 't3_arm_9f89a23.log'
t4_glued: FAIL — 1 glued lines (writes not whole-line atomic)
rc=1

LINE 1 tags=2: [KERNEL] pid=[ARRIVED] SH.BIN rc>0
   tag positions: [0, 13]
total glued: 1
```

The culprit is identifiable and it is not subtle. `[KERNEL] pid=` is still a **multi-call** `uart_puts`
sequence — `src/kernel/arch/arm/trap.c:2117-2135` — the data-abort kill record, untouched by #141:

```c
      uart_puts("[KERNEL] pid=");
      print_int(gcur->pid);
      uart_puts(" (");
      uart_puts(gcur->name);
      uart_puts(") memory fault VA=");
      uart_print_hex(far);
      ...                                    /* ~15 separate calls */
```

and the serial shows the whole record shredded across four lines by other writers:

```
216: [KERNEL] pid=[ARRIVED] SH.BIN rc>0
217: 7[WAVE-TRY] SMPTEST.BIN
218:  (Loading program for scheduler (v2 AS): MEMTEST.BINSMPTEST.BIN) memory fault VA=
219: 0x0000000040000000/SMPTEST.BIN PC=
```

The counter counts only lines carrying **two record tags**, so this wreckage scores 1 — line 218 carries
one tag and half of two other records. **The true damage is larger than the number.** On intel the analogous
site was converted to the `fl_s()` formatter (`arch/x64/trap.c:2597`); the ARM one was missed.

Consequence for the card: "ARM glued 0" holds only on runs that never take a data abort. This wave took five
(`memory fault` × 5), and the count is 1.

## Clause 2 — intel 0, or the 2 named — **2, both named**

```
t4_glued: 2 glued console lines in 't3_intel_9f89a23.log'
```

1. `[PIPE_WRITE_EPIPE] PID [KERNEL] Process 39: HEDGNU.BIN exited unexpectedly or gracefully.`
   Writer: `src/kernel/pipe.c:238` — `uart_puts("[PIPE_WRITE_EPIPE] PID ")` then `print_int`, then more
   calls. Another unconverted multi-call kernel record, gluing into `program_loader.c`'s kline exit record.
2. ` [SLTFANC]K [P3A]S:S  03x00+0 0c0h0a0r0 0c0o0m0p0o0n0e9n3t`
   De-interleaved: `[LFN] PASS 30+ char component ` (user program) **character-by-character** with
   ` STACK[3]: 0x0000000000000093` (kernel). This is worse than a fused line — two writers alternate single
   characters, so neither record is atomic even at byte granularity on intel. The counter's `STACK[` skip
   cannot fire because the text is mangled.

Neither is "close enough to zero". Writer 1 belongs to **this card** (same class, same fix pattern: kline).
Writer 2 needs the UART lock held across a whole record on the char path — I would give it its own card,
since it is a locking question, not a formatting one.

## Clause 3 — gate self-test + "the tier's own glued verdict" — **half PASS, half absent**

Self-test PASS in this session, both ways:

```
$ bash tools/gate/t4_glued.sh --self-test
self-test: glued fixture count=4 (want >0), clean fixture count=0 (want 0)
self-test: absent-log exit=2 (want non-zero)
self-test: OK — counter can fail, stays 0 on clean, and refuses a missing log
$ make t4_glued_selftest   → rc=0
```

But **there is no tier glued verdict to be green.** `t4_glued.sh` is referenced only by its own
`t4_glued_selftest` Makefile target — no call from `tools/t3_tier.sh`, `merge_gate.sh` or `parity_ci.sh`
(`05_no_tier_glued_verdict.txt`). So "the tier's own glued verdict green on both arches" cannot be satisfied
until something actually calls the counter; as it stands the number is quoted by hand, which is exactly the
"measured manually is not a gate" pattern this program keeps paying for.

## Clause 4 — the moved records still print — PASS

```
sys_fork       169      [ARP]  8      Before eret  258      memory fault  5
[KERNEL] sys_fork: tf->regs[0] is now 10
[ARP] routing off-subnet: target=0x0000000001010101 local_ip=0x000000000F02000A ...
```

No diagnostic was silently dropped by the conversion.

## DoD table

| # | clause | result |
|---|---|---|
| 1 | ARM = 0 on a halted=1 run, `t4_glued.sh` the only rule | **FAIL** — 1 glued line, from the unconverted multi-call abort record at `arch/arm/trap.c:2117` |
| 2 | intel = 0 or the 2 named | **2, named** — `pipe.c:238` (this card) and a char-level `[LFN]`/`STACK[` interleave (new card) |
| 3 | `t4_glued.sh --self-test` green + the tier's own verdict | self-test **PASS**; tier verdict **does not exist** |
| 4 | each moved debug record still prints | **PASS** (169 / 8 / 258 / 5) |

## What closes it

1. Convert the ARM data-abort record (`arch/arm/trap.c:2117-2135`) to the kline formatter, the way
   `arch/x64/trap.c:2597` already is, and convert `pipe.c:238` the same way.
2. Wire `t4_glued.sh` into `tools/t3_tier.sh` so the tier prints the count it scores — then the clause-3
   sentence means something, and a run that never takes a fault cannot be mistaken for a clean run.
3. Re-run both arches to `halted=1` and quote `t4_glued.sh` on each serial.

Out of scope, untouched: the `0x7` lock-pointer corruption (GB-096) and libc buffering (GB-094).
