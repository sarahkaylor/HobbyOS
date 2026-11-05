# GB-209 — the panic-sink violation record: contiguous, and one counter per record

Base: `gb097/panic-sink-code` @ `5c88129d` (PR #254's branch). Branch `ticket/209-sink-record`.
Re-run 2026-11-05, intel, `MODE=test`, default 8 CPUs (`QEMU_SMP` unset), COM2 captured with
`T3_QEMU_ARGS="-serial file:..."`.

## What changed (`src/kernel/arch/x64/uart.c`)

1. **Records, not characters.** `panic_sink_violation()` is entered once per character
   (`panic_putc`) and once per fragment (`panic_puts`). A `panic_sink_record_open` latch counts the
   first entry of a record and skips the rest until a `\n` is written through `panic_putc`/
   `panic_puts`, so `panic_sink_dropped` moves ~once per line, not once per byte.
2. **A sink-local claim.** Two returning CPUs can enter the reporter at once; a
   `panic_sink_claim` atomic exchange lets exactly one write the record (GB-097 clause 1.2's
   claim word is the fatal record's; this is the violation reporter's).
3. **The record goes to COM2**, which nothing else writes, so it cannot be byte-interleaved;
   each COM2 record carries `suppressed=N` with the record counter.
4. **One COM1 summary line per boot**, written under the sink-local claim. The old comment
   ("writing byte-wise here can itself shred one line, which is accepted") is deleted — that
   acceptance was the defect.
5. The allowlist entry for `uart.c` moves 20 → 26 references (the new COM2 record + COM1 summary).

**Measured dead end (why no `uart_lock`).** The first implementation wrote the COM1 summary under
`uart_lock` ("safe because `panic_depth == 0`"). In the wave a halted CPU owned the console lock:

```
[LOCKFOREVER] cpu=0 lock=0x0000000075581dd8 owner=1 owner_hb_age=4294967295 pc=0x000000007003d76e pc2=0x0000000070047cf9
```

The violator spun forever on a dead owner and the console stopped. Reverted to the sink-local
claim; the comment now says `uart_lock` is off-limits here.

## DoD 1 — contiguous records, no byte-interleaved copy

COM1 carries exactly one summary line per boot — 12 boots → 12 lines, all contiguous, no other
`PANIC-SINK` bytes anywhere in the serial (`grep -ac PANIC-SINK` = 12):

```
Original CCB: [PANIC-SINK-VIOLATION] panic_putc from a returning path (suppressed=1)
...(12 boots, one each)
```

`console_shred.py` on the T3 serial reports 2 hits — `L646` (a FATAL banner) and `L14216` (a
WATCHDOG line) — **neither is a `[PANIC-SINK-VIOLATION]` record** (`shred_t3_excerpt.log`).

## DoD 2 — the counter counts records

COM2: `records=16855` across the 12 boots; the last boot's counter runs `suppressed=1,2,3 … 1800`
— one increment per record. Before this change the same path printed 78,781 `[PANIC-SINK]
suppressed=N` lines for one returning-path diagnostic (V2), i.e. per character.

```
[PANIC-SINK-VIOLATION] panic_putc from a returning path suppressed=1
[PANIC-SINK-VIOLATION] panic_putc from a returning path suppressed=2
[PANIC-SINK-VIOLATION] panic_putc from a returning path suppressed=3
...
[PANIC-SINK-VIOLATION] panic_puts from a returning path suppressed=1221: [KERNEL] released proc_lock before halting (GB-0
[PANIC-SINK-VIOLATION] panic_putc from a returning path suppressed=1800
```

## DoD 3 — suite / links / T3 wave

```
make host_tests -j1   -> Checks run: 482 / Checks failed: 0 / TEST EXIT: 0
intel link            -> rc=0
arm link              -> rc=0
```

The default intel T3 wave's COM1 serial has **zero shredded `[PANIC-SINK-VIOLATION]` lines**
(2 detector hits total, both other records). The wave itself is RED at this head — `T3_intel EXIT=1`
with `FPU_T FAILED: 2`, `THRD_T FAILED: 1`, `SQLTEST select-rows: FAIL`, and `FATAL kernel-mode
fault … vector=6` (GB-146's state); this card does not claim a green T3.

## Falsifier

Run the same returning-path diagnostic that produced the three shredded lines in the V2 serial:
if any `[PANIC-SINK-VIOLATION]` record is byte-interleaved, or `suppressed` grows per character,
RED. Measured here: 12 boots, 12 contiguous summaries, counter 1→1800 by record.

## Evidence files

`com1_summary_excerpt.log`, `com2_record_excerpt.log`, `shred_t3_excerpt.log`,
`host_tests_excerpt.log`, `links.log`, `t3_verdict_excerpt.log`. Full serials:
`/home/sarah/.hermes/cache/scratch/{t3_gb209b.log,com2_gb209b.log}` (scratch host).
