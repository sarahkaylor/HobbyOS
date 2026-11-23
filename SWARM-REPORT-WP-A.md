# SWARM-REPORT-WP-A — FWSEC launch + fuse check (RTX 4090 `10de:2684`, AD102 `0x192`)

Branch `nv/wp-a-fwsec`, base `ce4c44af`, commit `35eb2d17`. Eight deploys against the card
(`tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`, run on the c4 box), logs committed
under `evidence/nv/`.

**Headline.** The plan's three branches are now separable, and the evidence picks two of them:

| branch | verdict |
|---|---|
| **the image is invalid for this part's fuses** | **RULED OUT (MEASURED).** `fuse@0x8241E0 = 1`, `SignatureVersions = 0x3`, common version found, selected signature index 1. `[NV-FUSE] verdict=COMPATIBLE` |
| **transport / encoding wrong** | **WAS WRONG (MEASURED, fixed).** Four defects: destination never advanced, 4-byte SIZE field, completion polled on the wrong register, and the RISC-V core select never cleared. After the fixes the image lands: `f1 verdict=POSITIVE 0x114=0xF100` |
| **ucode-or-params wrong** | **ALSO WRONG (partly fixed).** The fuse-selected signature was not the one patched, and the FRTS command was never written into the DMEM image. Both fixed. **But `0x1438`'s hi16 stopped reading `0xBEEF` only after the transport fix**, which is where the wall actually was |

With those six defects fixed the FWSEC channel answers for the first time in this project's
history: `[NV-FWSEC] status1438=0x0000000000000000 … frts=0 (ucode ran)`. **The launch is
still not complete**: SEC2 remains fenced and the core does not halt. That is stated as
incomplete below, not rounded up to a pass.

---

## 1. Per-experiment verdicts

| # | experiment | verdict | the log line that decides it |
|---|---|---|---|
| **F1** | read GSP `0x110114` after the IMEM load | **MASKED, then POSITIVE.** The register answers the fence word while the core is selected as RISC-V, and it is only **24 bits** wide. After the select clear it answers properly. | `R1`: `[NV-PROBE] @0x110114 before=0xBADF5620 wrote=0xC0FFEE00 back=0xBADF5620 … NOT-READ-WRITE` · `R7`: `[NV-PROBE] @0x110114 before=0x0 wrote=0xC0FFEE00 back=0x0FFEE00` · `R8`: `[NV-FWSEC] f1 verdict=POSITIVE 0x114=0xF100` |
| **F1 (encoder side)** | what destination the host actually programmed | **NEGATIVE before the fix**: every one of 256 blocks was addressed at falcon offset 0. | `R1`: `[NV-FWSEC] f1 requested dst first=0x0 last=0x0 blocks=256` → `R2`: `… last=0xF100 blocks=242` |
| **F2** | load by PIO, no DMA request, no context, no status poll | **NEGATIVE — and the most valuable negative.** The image went in by PIO and FWSEC still wrote nothing, so the DMA *encoding* was not the only cause. | `R4`: `[NV-FWSEC] f2 transport=PIO (port 0: 0x180/0x184 IMEM with SECRET, 0x1c0/0x1c4 DMEM; …)` / `f2 imem pio blocks=242 bytes=0xF200` then `status1438=0xBEEF0000 … frts=NEVER WRITTEN` |
| **F3** | `0x111668` after the attempt | **NOT SWITCHED.** `0x110` = RISCV\|BRFETCH with **VALID clear**; `0x111` never appeared. Clearing the select *after the reset* is what un-fences `0x110..0x120` and makes STARTCPU take. | `R1`: `f3 bcr_ctrl(0x111668)=0x110` (my first verdict text read this as "switch taken" — **wrong**, VALID was clear; corrected in `R5`) · `R5`: `[NV-FWSEC] f3 core-select cleared: 0x111668 0x110 -> 0x1 (VALID: the falcon core is now the selected core)` |
| **F4** | `0x840100` (privilege drop) | **STILL FENCED in all 8 runs** — FWSEC's job is not done. | `[NV-FWSEC] f4 sec2 0x840100=0xBADF5620 0x840000=0xBADF5620 verdict=STILL FENCED` |
| **F5** | vary the DMA context id | **RULED OUT — by F2, without spending a deploy.** PIO transfers pass no context at all and produced the identical failure, so a context mismatch cannot be the cause. The plan's premise is also wrong on the source: `0x600` is **one** `TRANSCFG` register whose *context* is a field in bit 16 (`mask 0x00010007 ← 0x5`), not an array indexed by register number; `0x5` is exactly what the reference writes, and the driver already wrote `0x5`. | `R5`: `[NV-FWSEC] dma cfg 624=0x190 10c=0x0 600=0x5` (readable only after the select clear) |
| **B** | fuse check: `0x8241C0 + (UcodeId-1)*4` AND `SignatureVersions` | **POSITIVE.** The image is valid for this part's fuses, and the fused version selects **signature index 1**, not 0. | `R2`: `[NV-FUSE] ucode=9 fuse@0x8241E0=0x1 sigvers=0x3 sigcount=2 and=0x1 sig_index=1` / `verdict=COMPATIBLE` |

One correction to my own first fuse row: in `R1` the check read an 8-dword block and indexed
it with `ucode_id - 1 = 8`, i.e. **one past its own array**, and reported
`verdict=NO-FUSED-VERSION` from that. `0x8241C0 + 8*4 = 0x8241E0` is the ninth dword. `R2`
reads the register explicitly and the answer is `0x1`. The bad row is left in the log above
rather than edited out.

## 2. What the six defects were, and what now proves each is fixed

| # | defect | proof it is fixed |
|---|---|---|
| 1 | `0x114` (destination) written as 0 for all 256 blocks, so a 0xF200-byte image left only its last 256 bytes at offset 0 | `f1 requested dst … last=0xF100 blocks=242` + `0x114=0xF100` read back (`R8`) |
| 2 | SIZE field `8` masks to `0` = a **4-byte** transfer per block; the engine accepts it silently | command word now from `nv_flcn_xfer_cmd(256,…)` = `0x614`/`0x600`; `0x118` reads back `0x616` post-load (`R8`), and the unit tier pins `0x614` |
| 3 | completion polled on `0x120` bit 1; the engine reports it on **`0x118` bit 1** (`R7` aborted block 0 with `-110` once the window became readable) | `R8`: `dma state post-imem … 118=0x616 … 120=0x10` and the full load completes |
| 4 | the RISC-V core select (`0x111668`) never cleared → STARTCPU did not reach the falcon core, and the DMA window read as fenced | `R5`: `0x110 -> 0x1`, then `after start ctl_1us=0x0 ctl_late=0x0 ctl_before=0x10` (halted → **running**) |
| 5 | signature index 0 patched unconditionally although the fuse selects 1 | `sig fuse=0x1 selects index=1` / `PKC signature patched at DMEM 0xB24 from desc+0x1AC head=15c26bac` (`R3`) |
| 6 | FRTS command never patched (`init_cmd` stayed 0, no region described) | `frts result=0 readback=0 init_cmd=0x15` + the 11 command dwords, e.g. `… 0x5FE300 0x100 0x2` (`R3`) |

Also fixed while there: `FALCON_RM (0x084) <- PMC_BOOT_0` (every reference falcon enable does
it), and the reset's handshake is no longer reported from a hard-coded `1` — `0x0F4` is inside
the read-fenced span, so the row now prints what it observed instead of claiming
`reset done=1 scrubbed=1` from a default.

## 3. Gate lines

```
Tests run:    517
Tests failed: 0
UNIT TESTS PASSED
```
517 = the 513-test baseline + 4 new cases (`nv_flcn_xfer_cmd`, `nv_fwsec_sig_index`,
`nv_fwsec_patch_frts`, `nv_fwsec_patch_refusals`) — the four encodings that cost these
deploys, pinned offline in `src/kernel/nv_flcn_test.c`.

Three of those cases failed while they were being written, and the failures were real
arithmetic/layout errors caught by the test rather than by a card run: `0x644` vs `0x614`
(SEC is bits 3:2, not 4:4), `w[4]` vs `w[5]` (the 8-byte `read_vbios.addr` occupies two
dwords), and the reset write-trail expectation after FALCON_RM/`0x114` probes were added.
The old encoding is refuted inside the test itself:
`ASSERT(nv_flcn_xfer_cmd(256,1,1) != (8&7)<<8 | 0x10 | 0x4)`.

ARM gate = per-TU compile of every changed file (the full ARM link is a **known pre-existing
failure at `ce4c44af`**: `ld.lld: error: kernel .bss exceeds the limine placement window`,
reproduced on an untouched tree — not addressed here):

```
clang … --target=aarch64-none-elf … -mgeneral-regs-only -c src/kernel/nv_gsp.c
clang … -c src/kernel/nv_pci.c
clang … -c src/kernel/nv_test.c
clang … -c src/kernel/nv_flcn.c
clang … -c src/kernel/nv_flcn_test.c
clang … -c src/kernel/unit_test.c
```
All six clean, no warnings (`nv_flcn.c` is deliberately architecture-independent, so the ARM
build compiling it proves it carries no x86-only reference).

## 4. What is NOT verified

1. **FWSEC has not demonstrably finished.** `0x1438 = 0x0` is the plan's own positive for the
   FRTS channel (`hi16 == 0`), and it changed from `0xBEEF0000` → `0x0` in exactly the run
   where the image first landed — but (a) it is one run, (b) that run did not log the
   sentinel's readback immediately before START (the readback loop's first read is the one
   that found 0), and (c) **SEC2 has not unfenced**, which is what FWSEC exists to cause.
   Treat "FWSEC wrote a zero FRTS status" as MEASURED and "FWSEC ran correctly" as INFERRED.
2. **The DMEM read-back disagrees with the staged buffer** and I could not resolve why:
   `init_cmd=0x34D93D3C/0x15 frts@d40=0x30/0x1 sig=0xB7C69B14/0xAC6BC215 → verdict=DMEM
   MISMATCH`. Either the load is not landing where I think, or the PIO *read* addressing is
   the "word index vs byte offset" ambiguity the envytools note describes (writes with base 0
   agree under both readings; a read at `0xB0C` does not). Until that is settled the read-back
   is not evidence either way, and it is the first thing to fix.
3. **The core never halts.** `halt wait done halted=0`, sampled still running at +1 s
   (`still running at +0x3B9F1AE0 ns: ctl=0x0`). What it is doing is unknown: `TRACEPC`
   (`0x148`/`0x14c`) reads the fence word, so the only remaining host-side view is closed.
4. **Why the fence lifts.** Clearing `0x111668` un-fenced `0x110..0x120` in one write; that
   was not predicted by any of the five contract docs, and the mechanism is not established.
5. **The BROM parameter writes are unproven.** `0x111180` reads the fence word, so the
   write-lands probe cannot say whether MOD_SEL/ENGIDMASK/UCODE_ID/PARAADDR landed. The
   contract's D4 stays open.
6. **The PIO path's IMEM SECRET handling is untested** (it ran, but nothing confirms the code
   arrived intact); the DMA path is the one now enabled.

## 5. Files

- `src/kernel/nv_gsp.c` — transport (destination, command word, done bit, pacing), falcon
  state (select clear, FALCON_RM), image patches (signature index, FRTS), the `[NV-FUSE]`
  check, and one `[NV-*]` row per experiment.
- `src/kernel/nv_flcn.c`, `src/include/nv_flcn.h` (new) — pure encoders: XFER command word,
  signature index from fuse + descriptor, FRTS/appif walk and patch, read-back.
- `src/kernel/nv_flcn_test.c` (new), `src/kernel/unit_test.{c,h}` — 4 kernel-unit cases.
- `src/kernel/nv_pci.c` — parse `InterfaceOffset`, `SignatureVersions`, `SignatureCount`;
  **add the missing `return 0`** at the end of `nv_vbios_probe()` (it fell off a non-void
  function).
- `src/kernel/nv_test.c` — fake model: `BCR_CTRL` with the measured `0x110`, `PMC_BOOT_0`
  capture, and the reset-trail assertions extended to cover the probes and the two fixups.
- `src/include/nv.h` — three descriptor fields.
- `plan.md` §2.1 — the measured results, replacing the guesses.
- `evidence/nv/*.log` — 8 serial logs, one per deploy.

## 6. Next step (named by the evidence, in order)

1. **Settle the DMEM read-back addressing** — read DMEM at `0xB0C` both ways (`0x2C3` as a
   word index vs `0xB0C` as a byte offset) and compare with the staged `init_cmd = 0x15`.
   Whichever disagrees is the bug, and it converts the read-back into the ground truth that
   defect 1–3 above currently lack.
2. **Then chase the halt.** With a trustworthy read-back: if DMEM holds the image and the core
   still never halts, the remaining candidates are the unprovable BROM parameters (§4.5) and
   the signature the ROM is actually verifying. A cheap discriminator: re-run with signature
   index 0 (one constant) — if the halt behaviour changes, the ROM is reading the signature,
   which also proves the four BROM parameter writes land.
3. Only after FWSEC halts with `0x1438 hi16 == 0` **and** `0x840100 != 0xBADF5620` does the
   plan's sequencing rule allow the SEC2 booter (`nv_gsp_sec2_start_booter`, already coded).
   It must not be started before that.
