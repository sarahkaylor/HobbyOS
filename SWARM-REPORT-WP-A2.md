# SWARM-REPORT-WP-A2 — FWSEC: did it run, and what the DMEM read path was really doing

Branch `nv/wp-a2-fwsec2` (cut from `d44659a2`). Six deploys of ten, each with a
named log under `evidence/nv/`. Card released: VM 300 and VM 312 both `stopped`
at the end of the round (`qm status 300` / `qm status 312`).

Everything below cites a serial-log line. Tags: **MEASURED** (seen in a log this
round), **SOURCE** (from a contract doc or another driver's published source),
**INFERRED** (a conclusion from the above, not an observation).

---

## 1. The headline

| Question | Answer | Tag |
|---|---|---|
| Did the FWSEC image reach the falcon's memory? | **Yes — DMEM word for word, 9 of 9 probes, in three runs.** IMEM is still unreadable by any path this driver has. | MEASURED (DMEM) / NOT VERIFIED (IMEM) |
| Did the ucode execute? | **Yes — the loaded image is what produces the post-START signature.** Remove the IMEM load and START is refused and nothing happens. | MEASURED by differential ablation (r2f) |
| Did FWSEC finish its job? | **No sign of it.** SEC2's window is fenced in all six runs, and the one channel that does not need it (the metadata `verified` word) is never written — even by a forced booter attempt. | MEASURED (negative) |
| What was round 1's `DMEM MISMATCH`? | The read path, not the load: the PIO index register was written one shift too far, so every read addressed a quarter of the intended byte offset. | MEASURED + fixed, with two red mutations |

So: **FWSEC's code runs, and the round-1 verdict word was a real field write, not
a clear — but FWSEC does not reach the state that unfences SEC2.** "FWSEC ran" is
now supported for *execution*; "FWSEC completed" is not, and the difference is
what the next round has to attack.

## 2. The defect that round 1 could not see

`nv_flcn_idx_word()` (new, `src/kernel/nv_flcn.c`): the PIO index register carries
the address as a **word index in bits 15:2**. The driver wrote `addr >> 2` into
bits 0..13 — one shift too many — so the register held `word/4`, i.e. every read
and every PIO write addressed **byte `addr/4`**.

Why it survived: the same mistake on the write side makes a write/read round trip
at the same (wrong) address still match. This driver's `nv_selftest` round trip
never had a chance. Only comparing against the *staged bytes* catches it, which is
what the landing map now does.

| Evidence | Line |
|---|---|
| Read path proved in-run before use, same byte offsets | `[NV-DMEM] path verdict=READ-PATH-TRUSTWORTHY (write then read at the same byte offset returns the written word)` |
| The load was right all along | `[NV-LAND] dmem @0xB0C got=0x15 want=0x15 MATCH`, `… @0xB24 got=0xAC6BC215 …`, `[NV-LAND] dmem probes=9 match=9 displaced=0 failed=0 verdict=LANDED` |
| What round 1 actually read | `init_cmd=0x34D93D3C/0x15` (r1) vs `got=0x15 want=0x15` (r2a, same address) |
| The 14-bit field used to be masked, not refused | `nv_falcon_dmem_read(0x10000)` returned DMEM word 0 with rc 0; now `-ERANGE` |

Tier evidence, both mutations watched red (`evidence/nv/r2-red-*.log`):

| Mutation | Case that failed |
|---|---|
| index written unshifted (round 1's form) | `test_nv_dmem_addr_units`: `fg.data_idx & 0xFFFC == 0xB0C` |
| the range refusal replaced by the old mask | `test_nv_dmem_addr_units`, `test_nv_dmem_probe`, `test_nv_flcn_idx_word` — 3 assertions |

## 3. The runs

| # | Log | What it was for | Result |
|---|---|---|---|
| r2a | `r2a-readpath-landing.log` | read-path self-test, landing map, sentinel change | DMEM 9/9 MATCH; `1438 0x5EC20000 -> 0x0 at +0x63E1540 ns ctl=0x0`; **IMEM reads constant `0xDEAD5EC1`** |
| r2b | `r2b-imem-store-sentinel.log` | low-half sentinel, 0x1400 block control, IMEM store-back | `1438 0x5EC20000 -> 0x00005A5A at +0x11F730C0 ns ctl=0x10`; `block1400 … changed_words=2 first@0x1418 was=0x81000`; store-back returned the marker |
| r2c | `r2c-imem-identity.log` | IMEM write probe (port + engine) | port does **not** read back a pattern it accepted (`pio_back=0xDEAD5EC1`); the store left `0x118=0x637` (FULL stuck) and **the load timed out** (`stage6b result=-110`). Answers the IMEM question; no launch data. |
| r2d | `r2d-sec2-forced.log` | forced SEC2 booter attempt | load still broken by the store (`result=-110`); `[NV-SEC2] meta verified=0x5EC2A0A0 … verdict=STILL THE SENTINEL`. A run that mostly re-taught r2c's lesson. |
| r2e | `r2e-fwsec-restored-sec2-force.log` | probe off, full load, forced SEC2 | DMEM 9/9; `1438 -> 0x00005A5A at +0x63E0860 ns ctl=0x0`; `dmem-samples … fenced=1 at +0xDA0 ns reads=1` (**the window closes 3.5 µs after START**); booter `verified` still the sentinel |
| r2f | `r2f-ablation-no-imem.log` | **ablation: IMEM not loaded** | `halt wait done halted=1 sentinel_changed=0`; `ctl_1us=0x10 ctl_late=0x10` (START refused); `status1438=0x5EC25A5A` untouched; `changed_words=0`; IMEM reads `0x0` instead of the marker |

## 4. The 0x1438 verdict word, classified

What the six runs establish, in order of how much it costs to be wrong:

1. **It is a field write, not a clear.** With a non-zero low half (`0x5EC25A5A`),
   the result was `0x00005A5A`: bits 31:16 were written to 0 and bits 15:0 kept.
   A region clear would have taken the low half too. **MEASURED** (r2b, r2e).
2. **It is not a neighbour of a block clear.** The only other change in the
   0x1400 block was `0x1418` `0x81000 -> 0x81800` (a +0x800 counter), which is
   movement, not clearing. A whole-block snapshot was the control — writing a
   sentinel into MAILBOX0 for that job was wrong twice over: the contract says
   `0x040 == 0` is part of completion, so a sentinel there reads as an error code
   (`docs/nv-devinit-contract.md` §3). **MEASURED** (r2b, r2e) / **SOURCE**.
3. **It arrives ~105 ms after START, well after the window closed.** START is
   consumed immediately (`ctl 0x10 -> 0x0` at 1 µs), the GSP window fences at
   3.5 µs, and the status field goes to 0 at 104.7–104.9 ms in two independent
   runs. **MEASURED** (r2e, r2a).
4. **It is caused by executing the loaded image.** With IMEM empty the core does
   not even start: `ctl` reads `0x10` at 1 µs *and* at the end of the wait, the
   status word keeps its sentinel, and nothing in the block moves. With the
   image loaded the same code path produces all three effects. **MEASURED**
   (r2f vs r2e).

What it still does not say: whether `0` means "FWSEC succeeded" or "the last
stage that ran wrote 0". The code values are not published, and the one effect
the contract names for success — SEC2 unfencing — never happens.

## 5. IMEM: what is and is not knowable

| Path | Result | Conclusion |
|---|---|---|
| PIO code port, SECRET set, before/after `CODE_VIRT_ADDR`, and with SECRET clear | `0xDEAD5EC1` at every address (9 rows) | a marker, not content |
| PIO code port, pattern written then read back | `pio_back=0xDEAD5EC1` | the port cannot confirm IMEM content |
| Same port with IMEM never loaded (r2f) | `0x00000000` at every address | the marker appears *because the page was loaded* — it is the read-back shape of a loaded secure page |
| XFER engine STORE direction (ours) | copies the marker in r2b; leaves the queue FULL and breaks the next load in r2c/r2d | unusable as encoded; compiled out (`NV_IMEM_STORE_PROBE 0`) |

So IMEM's *bytes* remain unverified, and this round does not pretend otherwise.
What the ablation does establish is the thing that matters for the verdict: the
image is not inert — an empty IMEM cannot start the core, a loaded one can.

## 6. What is proven, refuted, unverified

**Proven (MEASURED)**
- The PIO byte→word conversion was wrong by a factor of four and is now fixed;
  the DMEM half of the image is in the falcon word for word, three runs.
- `0x1438` bits 31:16 are written to 0 as a field (low half preserved), ~105 ms
  after START, while no other word of the 0x1400 block is cleared.
- The GSP window fences **3.5 µs** after START when the code is loaded (and does
  not fence when the core never starts) — the launch's unobservability is a
  consequence of execution, not of the reset.
- START is *refused* when IMEM is empty, and consumed when it is loaded.
- SEC2's window is fenced at the end of all six runs; the metadata `verified`
  word stays at its sentinel even when the booter is attempted behind the fence.
- The SEC2 mailboxes read back the host's own writes (`mailbox0=0x054E2000`),
  so a booter status would be visible there if one were ever produced.

**Refuted**
- Round 1's reading that the image "did not land" because DMEM disagreed: the
  image landed; the instrument was pointed at `addr/4`.
- "The core cleared the verdict word on its way down" as a *clear*: the low half
  survived and no neighbour changed.
- "STARTCPU never takes": it takes, and `ctl 0x10 -> 0x0` is the proof; it is
  refused only with an empty IMEM.
- "A code-port write is harmless instrumentation": it stops the engine collecting
  the next DMA request, twice (r2c, r2d).

**Not verified**
- The bytes in IMEM (no working read path).
- Whether FWSEC's `0` is success or a stage code; and why the privilege drop that
  should follow it does not happen.
- What `0x1418` is (advances by 0x800 once per launch).
- Whether SEC2's fence also drops writes (the forced attempt wrote into it and
  produced no effect, which is consistent with either).

## 7. Next step

The observation window closes 3.5 µs after START, so the next round has to either
open a channel that survives that, or read the state afterwards through the two
paths that still answer (`0x1438`, the SEC2 metadata block).

1. **Fix the engine STORE encoding** (the only way to read DMEM/IMEM after the
   fence). Ours leaves `0x118` FULL stuck; the likely error is which side owns
   `0x11C` on a store, or that a store needs the same `SEC` field the load used.
   A one-block store into a *scratch* DMEM address with a prefill that cannot
   coincide with the answer (`0xA6A6A6A6`, not `0x5A5A5A5A` — r2c's prefill was
   ambiguous) settles it without another launch.
2. **Time the window, not just the value.** Poll `0x110100`/`0x111668`/`0x1438`
   non-blocking for the full 2 s and log the transitions: if the GSP window
   re-opens at ~105 ms — the same instant the status is written — then FWSEC
   finished then, and DMEM (via step 1) can be read at that moment to see what it
   actually produced.
3. Only after that, the SEC2 fence: the forced booter attempt is coded and
   measured negative twice; it is not worth repeating until FWSEC's own state can
   be read.

## 8. Gate status

| Gate | Result |
|---|---|
| `nvswarm build wp-a2` (intel) | green |
| unit tier | **566 run / 0 failed** (`evidence/nv/r2g-unit-tier-566.log`; 561 at the cut + 5 new) |
| own suite fails before the fix | two mutations, three failing assertions, logs committed |
| ARM per-TU compiles of every touched file | clean (`evidence/nv/r2-arm-tus.log`) |
| full ARM link | still the pre-existing `.bss` failure, `wp-arm`'s job |
| card left clean | VM 300 `stopped`, VM 312 `stopped` |

Files changed: `src/include/nv.h`, `src/include/nv_flcn.h`, `src/kernel/nv_flcn.c`,
`src/kernel/nv_falcon.c`, `src/kernel/nv_gsp.c`, `src/kernel/nv_regs.h`,
`src/kernel/nv_test.c`, `src/kernel/nv_flcn_test.c` — plus evidence logs.
