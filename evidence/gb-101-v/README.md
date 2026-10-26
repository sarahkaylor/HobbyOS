# GB-101-V — gate evidence (VERDICT: RED, clause 2 of 5)

Ticket: GB-101-V (#182) — Verify the torn-frame detector: injected frames above and below the
`vector < 32` boundary must be caught by name
Verifies: GB-101 (#180), PR **#187** (`ticket/101-trapframe` @ `83a6b5b`, one file, +25 lines in
`src/kernel/arch/x64/trap.c`), base `origin/main` @ `ee7b176`.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2), host `swarm-farm-2`.
Run 2026-10-26 ~05:56–07:30Z. Independence: I authored neither GB-101 nor PR #187.
Local gate only — no GitHub check/status result is cited, per the 2026-10-26 CI policy.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | police PASS on a clean log, FAIL on the same log + injected torn frame, naming `[TORNFRAME]`, with `vector=` and `rip=` | **PASS** |
| 2 | detector sits at handler ENTRY; injections **above 32** and **below 32** both caught by name | placement **PASS**, above-32 **PASS**, **below-32 FAIL** |
| 3 | three consecutive intel runs, counts quoted separately (TORNFRAME / System halt / FATAL with vectors) | 2 completed + 1 aborted — counts below, stated as such |
| 4 | on detection: emit, `cpu_halt_record()`, release `proc_lock` if held, stop the CPU; no dispatch after | **PASS** (code path quoted) |
| 5 | `make ARCH=arm MODE=unit_tests hobbyos.elf` links | **PASS** (`ARM_LINK_EXIT=0`) |

## Clause 1 PASS — the classifier discriminates

`tools/t3_tier.sh police` over one log, clean and with one `[TORNFRAME]` line injected just before the
halt (`police_control.sh`, logs `ctl_*.log`, `police_*.txt`):

```
=== clean
T3_intel (police evidence/gb-101-v/ctl_base.log): 1202 lines, 0 FAIL lines, … halted=1, kfault=0, boots=1
T3_intel EXIT=0
=== torn_4E20
T3_intel (police …/ctl_torn_4E20.log): 1203 lines, 1 FAIL lines, … halted=1
1202:[TORNFRAME] FAIL - interrupt/fault handler read an impossible vector=20000 (x86 IDT has 256 gates; >0xFF means the trap frame is corrupt, not a real exception) rip=0x97
T3_intel EXIT=1
```

Baseline is synthetic and labelled: no captured intel log on this box is both above the 1000-line floor
and free of the known GB-093/095 FAIL rows, and `police` is a log classifier — the real-guest half is
clause 3.

## Clause 2 — placement PASS, above-32 PASS, **below-32 FAIL**

The check is at the top of `general_interrupt_inner`, i.e. **before** `gic_set_current_vector` and
`watchdog_tick`, and every IDT gate's stub reaches it (`trap.c:3444 "call general_interrupt_handler"`,
one asm-visible entry):

```c
if ((uint64_t)tf->vector > 0xFFu) {
  ... uart_puts_raw("[TORNFRAME] FAIL - … impossible vector="); ...
  cpu_halt_record(cpu); ... while (1);
}
```

Both above-32 injections fail by name (`0x4E20` = 20000, `0x7703A33E` = 1996485438 — the second measured
shape from the parent card).

**A torn frame with a garbage vector below 32 cannot be caught by this condition**, and the torn frames in
this family carry exactly that shape — the parent gate ticket's own body (2026-10-26 05:16Z) quotes the
owner's run as recording `vector=13 rip=0x7009`, `vector=6 rip=0x7`, `vector=6 rip=0x1000007` alongside
the two large ones. With `> 0xFF` those frames pass the guard and reach `gic_set_current_vector` and the
normal dispatch: the detector is silent for them. The classifier control above shows the *police* half
fails such a line, but the kernel can never emit it.

So the card's clause 2 second half is unmet: the detector covers impossible *values*, not corrupted
*frames* below the 0xFF boundary. Fix direction (owner's call): a frame-coherence test — e.g. `cs`/`ss`
consistency, `rip` inside a mapped user/kernel range, or `elr`/`vector` cross-check — not a wider
threshold.

## Clause 3 — two completed runs, one aborted (stated, not hidden)

`T3_MONITOR=1 T3_TIMEOUT=1500 bash tools/t3_tier.sh intel`, counts grepped from each run's own serial:

| run | lines | `[TORNFRAME]` | `System halt` | `FATAL kernel-mode fault` | boots |
|---|---|---|---|---|---|
| 1 | 21 837 | **0** | **0** | 1 — `vector=6 rip=0x1039E52` (legible) | 4 |
| 2 | 39 288 | **0** | **0** | 1 — record shredded by console interleaving, vector not legible | 5 |
| 4 | 17 968 | **0** | **1** | 1 — record shredded (line 11449 interleaved with `[KERNEL] Process 35: TACTO.BIN exited…`), vector not legible | 2 |
| 3 | 15 672 | 0 | 0 | 0 | 2 — **aborted**: `RUN3_EXIT=141`, tail `qemu-system-x86_64: terminating on signal 15`, no verdict lines; not counted |

Three completed runs, one aborted and excluded. Clause 1 of the card (TORNFRAME = 0 expected) holds in all
three: **the detector was silent and two of the three waves still did not halt** — the card says that is
the honest, expected result and must not be graded as this card's failure. Reported separately as it
requires: TORNFRAME 0/0/0, halted 0/0/1, FATAL 1/1/1, with `vector=6` legible in run 1 only.

Run 1's legible record is the useful one for clause 2: the wave's fatal fault carried **`vector=6`**, a
value the `> 0xFF` guard cannot reject — the exact below-32 shape.

Two things stated as observations, not as this card's verdict:
* **`boots=4/5/2`** — the tier's police rule treats more than two `Controller reset complete` lines as
  the guest resetting mid-run (a triple fault has no handler), so runs 1 and 2 reset several times before
  wedging.
* **Run 4's single FATAL record is not legible** (`deinterleave.py` could not cleanly separate the two
  writers; 24/65 characters matched, so the stream has dropped/duplicated characters — the GB-082 console
  family). Its `vector=` is therefore **not** claimed here. If it is in fact an impossible value, the
  absence of a `[TORNFRAME]` line in that run would be a hole above 0xFF as well, and it is worth one
  `T3_SMP=1` run to settle — single-CPU removes the cross-writer interleaving that makes this unreadable.
  I did not spend that run inside this card's gate.

## Clause 4 PASS — the code path, quoted from the diff

```c
uart_puts_raw("[TORNFRAME] FAIL - …"); print_int_raw(...); uart_print_hex_raw(...);  /* (a) emit, lock-free */
cpu_halt_record(cpu);                                                              /* (b) GB-060 */
if (spinlock_release_if_held_by_me(&proc_lock)) uart_puts_raw("[KERNEL] released proc_lock …");
while (1);                                                                         /* (d) stop this CPU, no dispatch after */
```
`uart_puts_raw` → `uart_putc_body` is the lock-free raw sink, so the record cannot deadlock on
`uart_lock`; nothing dispatches after the record.

## Clause 5 PASS

```
$ make ARCH=arm MODE=unit_tests hobbyos.elf
ld.lld -T linker.ld -o hobbyos.elf obj/arm/... obj/arm/trap.o obj/arm/uart.o
ARM_LINK_EXIT=0
```

## Verdict

**RED**: clause 2's below-32 half is not implemented (the guard cannot fire for the measured
`vector=6`/`vector=13` torn frames), so a corrupted frame in that shape is dispatched exactly as before.
Clauses 1, 4, 5 pass and clause 3's measurement (TORNFRAME 0, halted 0) reproduces the parent's — the
detector is well-placed, lock-free and halts cleanly; it just answers a narrower question than the card
asks. GREEN needs the frame-coherence test (or the card's scope re-derived with the Coordinator).