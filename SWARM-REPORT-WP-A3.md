# SWARM-REPORT-WP-A3 — the read-path wall, and what FWSEC actually produced

Branch `nv/wp-a3-fwsec3`, cut from `756c8c9d` (round 2 merged, 578 tests).
Four deploys of ten used. VM 300 and VM 312 left **stopped**.

The question this round asked: *the engine store is the only channel that could read
falcon memory past the 3.5 µs fence — does it work, and if it does, what did FWSEC
produce between START and the verdict word at ~104 ms?*

Answer, in one line: **the store direction does not execute for host requests on this
part, the fence never re-opens, and the one thing left to read is nothing — but the
launch itself is now judged, and it got sharper: FWSEC accepts its FRTS command,
finishes it in 105.8 ms, and the privilege drop still does not happen.**

## 1. What changed

| File | Change |
|---|---|
| `src/include/nv_flcn.h` | the store direction as seven encodings + a field-swap axis (`nv_flcn_store_cmd/off/name`); `nv_flcn_read_class` (fence / sec-marker / dead / real); `nv_flcn_prefill_collides` |
| `src/kernel/nv_flcn.c` | those functions, pure, no MMIO |
| `src/kernel/nv_flcn_test.c` | 3 new cases (14 asserts on the encodings, 7 on the read classifier, 6 on the prefill ambiguity), 578 → **581** |
| `src/kernel/nv_gsp.c` | (a) `nv_store_block`/`nv_store_row`/`nv_store_matrix`: the store matrix, judged by **host memory** instead of a status register; (b) `nv_launch_sweep`: the blocking wait replaced by a 2 s non-blocking sweep with timestamps, a pre-START sweep with the same code path, and an instant snapshot at the verdict write; (c) the verdict channel's negative control (fault-injected `init_cmd`), compiled out with its result |

Gates, all on the c4 box:

```
$ TOOLS_SKIP=1 BROWSER_SKIP=1 make -j12 ARCH=intel MODE=unit_tests LO_ON_DISK= hobbyos.elf disk.img
rc=0   (only pre-existing warnings, none in nv_*)
$ UNIT_TESTS_TIMEOUT=600 ./run_unit_tests_intel.sh
Tests run:  581 / Tests failed: 0 / UNIT TESTS PASSED
$ make ARCH=arm obj/arm/{nv_flcn,nv_flcn_test,nv_gsp}.o   →  rc=0, 0, 0
```

A red run was watched, not assumed: `cmd |= NV_FLCN_XF_DIR` mutated to a wrong bit
turns `test_nv_flcn_store_encodings` red at its first assert
(`evidence/nv/r3-red-store-encoding.log`), and the tier reports `Tests failed!`.

## 2. The four runs, one question each

`NVSMI.BIN` wave, `--mode test --gpu`, `STAGE_FW=1`. Numbers are from the logs named.

| Run | Log | Asked | Answer |
|---|---|---|---|
| **r3a** | `r3a-store-and-sweep.log` | Does the store direction work, with a control that can fail? | **No, and not for want of encoding.** DIR-clear control moved 0xA6A6A6A6 into DMEM 0x1800 (`was=0 now=0xA6A6A6A6`) — the engine drives those addresses. All four DIR variants (PLAIN, CTX=5, owner bit 31 on 0x11C, both) put **0 of 64 words** in host memory, while `0x120`'s stores-pending count went **1, 2, 3, 4** — every request was queued into a 4-slot store queue and **none retired**; the queue never went FULL. |
| **r3b** | `r3b-clean-sweep.log` | Same launch, no store requests: does the verdict still arrive, and does the window re-open? | Verdict word `0x5EC25A5A → 0x00005A5A` at **+105,758,880 ns (105.759 ms)**, core halts at +105,754,880 ns (4.0 µs earlier = one sample interval). Sweep: 649,152 samples over 2 s, **1 event** (the halt), `dma114`/`bcr`/`pc` fenced from the first sample to the last, 40,572 DMEM samples **all** `0xBADF5620`. At the instant: 4/4 DMEM words FENCED. |
| **r3c** | `r3c-fence-bracket-and-r3a-control.log` | Bracket the fence with one instrument, and control r3a's confound (time vs stores) | Pre-START, 60 ms, 16,192 samples: **all four words real, 0 fenced, 1,012 DMEM samples answered**. First sample after START: fenced. Then 80,001,312 ns of doing nothing, and the verdict still arrived at **+105,788,608 ns** — so the delay is exonerated and **r3a's silent launch is attributable to the four queued store requests**. |
| **r3d** | `r3d-fault-injected-init-cmd.log` | Can the verdict word read *anything but* 0x…5A5A? (the channel's own negative control) | `init_cmd 0x15 → 0x99` at DMEM+0xAE0+0x2C, read back through `nv_fwsec_read_cmd` (rrc=0). Core halts at **+27,623,488 ns (27.6 ms)** and **0x1438 keeps its sentinel — nothing is written at all.** |

## 3. What that adds up to

**Proven (MEASURED, each row a log line):**

1. **There is no host-accessible read-back channel for falcon memory once the core is
   started**, and it is not a timing problem:
   - the window is open for at least the 60 ms before START (r3c: 16,192/16,192 samples real);
   - it is fenced at the first sample after START — within ~4 µs — and **no sample of
     any of the four words was real again in the following 2 s** (r3b 649,152; r3c
     653,376; r3d 658,496 iterations), **including the 1.89 s after the core halted**;
   - the DMEM port answered 40,572 / 40,836 / 41,156 times in those runs and returned
     the inaccessibility word every single time, and 4/4 words at the exact instant of
     the verdict write.
   So the fence is **not** a timer and **not** "the core is running": START relinquishes
   the window one-way. Post-mortem reads are impossible too.
2. **The store direction is not an encoding bug.** In r3a, with a control that works in
   the same run, five requests behave identically — PLAIN, ctx 5, owner bit 31 on 0x11C,
   both, and IMEM sec=1: the request is *accepted* into a 4-slot queue, the counter
   increments, nothing is ever retired and no byte reaches host memory. The DIR-clear
   control on the same addresses moves bytes, so the engine, the base/offset registers
   and the sizes are all right — the **direction** is what does not run.
   The field-swap variant and ctx 1/4 were issued only *after* the fence (r3b–r3d), where
   the same run's DIR-clear control could not answer either; those rows are
   **inconclusive, not negative**, and are not counted as evidence.
3. **A queued store costs the launch.** r3a (stores before START): core reads 0x0 at
   1 µs and never halts, verdict word never written. r3c (same ~80–100 ms before START,
   no engine request): verdict at 105.789 ms. One variable.
4. **FWSEC's verdict word is conditional evidence, not a status register.** With
   `init_cmd = 0x99` the core takes its exit at 27.6 ms and writes **nothing**; with the
   FRTS command it writes `0x00005A5A` at 105.8 ms. So the write means "the command was
   accepted and ran", and — new — **the halt time is the real verdict channel**:
   27.6 ms = command rejected, 105.8 ms = command completed.
5. **The hi16 = 0 field write still does not mean success.** No error code exists on
   this channel (r3d proves a rejected command writes none), so "hi16 = 0" is only
   "the handler finished". SEC2 reads `0xBADF5620` in **all four** runs: the privilege
   drop did not happen, and by this lane's rule the booter was **not** attempted.

**Refuted:**

- "The store encoding is wrong — the owner bit on 0x11C, or the ctx field, explains the
  wedge." Both were tested, in the same run, with a working control. Neither changes
  anything. r2c's stuck-FULL was the queue filling, not a malformed command.
- "Everything after 3.5 µs is invisible to a blocking poll, so a sweep will see the
  window re-open at ~104 ms." The sweep shows there is nothing to see: the window does
  not re-open, at 104 ms or at any other point.
- "A pause between the load and START might be what broke r3a." 80.0 ms of pause with
  no engine request reproduces the 105.8 ms verdict.

**Unverified (and why):**

- **What FWSEC wrote into DMEM/IMEM.** Both channels are closed (above). The image
  *lands* (`dmem probes=9 match=9` every run) but the code half rests on the DMA's
  effect only.
- **Whether hi16 = 0 means the FRTS work succeeded** — only that the handler ran. The
  `read_vbios.addr/size = 0/0` fields in the staged command are per the contract, and
  nothing in these runs says whether FWSEC needed a VBIOS image to validate.
- **The BAR1/VRAM route** (the FRTS region at `0x5FE300000` is outside the 1 GiB
  aperture this driver maps, and inside the WPR, which is protected): not attempted.
  It is the only candidate channel left that does not go through the falcon window.

## 4. Instruments, and two defects I am reporting against myself

- `[NV-STORE] … pending_s=` is printed from `0x120`, which is **inside the fence** after
  START: in r3b/r3c that row shows `pending_s=7` derived from `0xBADF5620` — a number
  that is the fence word, not a counter. The *judgement* in those rows (changed=0, 0/64
  words) comes from host memory and stands; the pending counters in post-START rows do not.
- `[NV-SWEEP] dmemchg` rows use one global "first sample" flag instead of a per-address
  baseline, so the first sample of each of the other three addresses reads as a change
  from 0. In r3c/r3d those three rows (`0xB0C 0x0 -> 0x15/0x99`, `0xD40 0x0 -> 0x1`,
  `0x000 0x0 -> 0x100001`) are **first-sample artifacts**, not changes. The post-START
  sweeps are unaffected (every sample there was fenced, so nothing was ever seeded) and
  `changes=0` in them is sound.
- `first_fence=+0` means "already fenced at the first sample", not "fenced at 0 ns"; the
  verdict row now says so.
- The store matrix and the 60 ms pre-START sweep are kept in the tree as permanent
  instruments (cheap, and each answers a question every run); the 80 ms pause and the
  fault injection are compiled out with their results recorded at the point of use.
- **The committed tip is not the binary that was last booted.** It differs from r3d by
  exactly those two reverts (pause removed, fault off); every element of it ran in
  r3b–r3d, but that exact combination has not been on the card. A first run of this tip
  should be read as "r3b plus the pre-START sweep", and its verdict time should land at
  ~105.8 ms if that reading is right.

## 5. Next step (highest leverage first)

1. **The wall to attack is the unfence, not the read path.** FWSEC completes its command
   (105.8 ms, hi16 written) and SEC2 still reads the inaccessibility word. The next
   experiment is on the *product* of that command, not on FWSEC's execution: read the
   FRTS region through BAR1 (`frts = 0x5FE300000`, 1 MiB) and the 256-byte WPR metadata
   block after a launch. The metadata's `verified` stayed at its sentinel in two forced
   attempts; if FWSEC's FRTS write is visible in VRAM, the hand-off is real and the
   blocker is SEC2's own gate; if VRAM reads are refused (WPR-protected), say so and
   stop treating VRAM as an oracle.
2. **Use the halt time as the verdict.** 27.6 ms = command rejected, 105.8 ms = command
   completed. Any future fault-injection run reads it in one number, without needing a
   code that does not exist.
3. **Do not use the engine store again before START** (it costs the launch): if a future
   round needs IMEM content, it needs a different channel, not a better encoding.
