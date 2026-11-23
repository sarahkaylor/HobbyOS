# The FWSEC launch contract on AD102 (RTX 4090, `0x192`) — and why the core does not execute

Branch `nv/lane-fwsec2`, base `6313df11`. Scope: the exact, implementable sequence for
starting FWSEC on the GSP falcon of AD102 from a non-vendor host, plus a differential
diagnosis for "STARTCPU written, MAILBOX0 untouched".

Method: three independent implementations of the same launch were read and reduced to the
contract they share — the vendor driver (`open-gpu-kernel-modules` 535.113.01, the version
this card's VBIOS is written for), nouveau (which boots a 4090 with the same 535.113.01
firmware), and the published register headers. Where the two implementations differ, the
difference is stated instead of being averaged. No code was copied; the tables below are
re-derived field lists with our own values.

Tags: **MEASURED** = read off this card (serial log or the dumped VBIOS), **SOURCE** = both
stacks or a published header agree, **INFERRED** = one stack only, or reasoned from other
facts, **NOT ESTABLISHED** = no source and no measurement.

---

## 0. Verdict first

Four independent defects sit between `[NV-FWSEC] DMEM loaded` and a core that runs. None of
them is the descriptor layout, which is what the last two attempts were aimed at.

| # | defect | evidence | tag |
|---|---|---|---|
| 1 | **The FWSEC image was never loaded into the GSP falcon.** `sec2_dma_block()` programs `NV_FALCON_SEC2_BASE` (`0x840000`), and every caller passes `dst = 0`. So all 484 IMEM blocks and 27 DMEM blocks were written into **SEC2's** falcon memory, all at offset 0. | `src/kernel/nv_gsp.c: sec2_dma_block()` line `nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_*)`; call sites `sec2_dma_block(d, phys, off, 0, 1, 1)` | MEASURED (code) |
| 2 | **Block size and stride disagree.** `XF_SIZE(6)` = 256 B, the loop advances 128 B, and `dst` never advances. Net effect even on the right engine: only the first 256 bytes of the image are ever written, and they are overwritten 484 times. | `for (off = 0; off < imem; off += blk)` with `blk = 128`; `XF_SIZE(6)` | MEASURED (code) |
| 3 | **No PKC signature in the DMEM image.** The descriptor's `PKCDataOffset` (`0xB24`) points at a **zero-filled** 384-byte slot in the ROM's DMEM image; both stacks copy a signature block from `desc+0x2C` over it *before* the DMA. Our driver does this for the SEC2 booter (`+0x10`, 384 B, from `booter+0x3C`) and not for FWSEC. | VBIOS dump: `dmem[0xB24..]` all zero; `pUcode->pSignatures ← pImage + descOffset + 44` | MEASURED + SOURCE |
| 4 | **Both BROM parameter values are wrong, by copy-paste from the booter path.** `0x111210 ← 0x10` must be `0xB24`; `0x11119C ← 1` must be `0x400`. | vendor `kgspExecuteHsFalcon_GA102`; nouveau `ga102_flcn_fw_boot` | SOURCE |

And one measurement error that makes the failure unreadable:

| # | defect | evidence | tag |
|---|---|---|---|
| 5 | **MAILBOX0 is not the FWSEC verdict channel.** The vendor passes `NULL` for both mailboxes when it launches FWSEC; nouveau writes 0 and checks for 0. "Mailbox never overwritten" therefore does not mean "the ucode did not run" — it may mean the ucode succeeded and never touched `0x040`. The channel the vendor trusts is `0x1438` bits 31:16 (plus the WPR2 pair, read-fenced here). | vendor `s_executeFwsec_TU102` → `kgspExecuteHsFalcon_HAL(..., NULL, NULL)`; `kgspExecuteFwsecFrts` reads `NV_PBUS_VBIOS_SCRATCH(0x0E)` | SOURCE |

Defects 1–2 mean the launch never had a chance; 3–4 would stop it even after 1–2 are fixed.
Defect 5 means the log line that produced the conclusion is not evidence of anything.

---

## 1. Who runs what, and what the host must hand over

| stage | runs on | host's job | tag |
|---|---|---|---|
| FWSEC-FRTS | **GSP falcon**, `0x110000`, falcon core (not RISC-V) | reset, DMA/PIO the image, patch 3 structures in DMEM, write 4 BROM registers, set BOOTVEC, START | SOURCE |
| FWSEC-SB | GSP falcon | teardown only (unload path). **Not** a prerequisite | SOURCE |
| booter | SEC2 | after FWSEC; gated on the privilege drop FWSEC causes | SOURCE |

FWSEC's payload is **signed**: `PKCDataOffset` plus 2 × 384-byte RSA-3K signatures in the
descriptor header, and the host is expected to move one of them into DMEM — nothing else about
the payload is the host's business. The 65408-byte image also has an entropy of 7.95 bits/byte
(MEASURED, `docs/nv-vbios-map.md` §6), so it is not line-for-line falcon code; the host does
not decrypt it either (INFERRED: the only key is fused into the chip). The core's Boot ROM
validates and installs it, which is why the BROM registers exist and why a missing or wrong
signature produces "started and stopped" rather than "wrong instruction".

---

## 2. The DMEM boot descriptor

### 2.1 The answer for AD102: there is none

| claim | status |
|---|---|
| GA102's FWSEC launch writes a `bl_dmem_desc` | **No.** `ga102_gsp_fwsec = {signature, reset, load=ga102_flcn_fw_load, boot=ga102_flcn_fw_boot}` — there is no `load_bld`, and `gm200_flcn_fw_load`'s descriptor branch is only taken `if (fw->boot)`. `fw->boot` is non-NULL only when a **separate `acr/bl` firmware** was loaded, which `nvkm_gsp_fwsec_v2` does (GA100, V2 descriptors) and `nvkm_gsp_fwsec_v3` does not (GA102, V3). | SOURCE (both stacks) |
| the vendor's GA102 path (`kgspExecuteHsFalcon_GA102`) writes one | **No.** It programs TRANSCFG, DMAs IMEM then DMEM, writes 4 BROM registers, sets BOOTVEC, mailboxes, STARTCPU. Nothing at DMEM offset 0. | SOURCE |
| therefore variant B is not a variant | It writes 21 dwords to `0x110180 + i*4`, which is **not DMEM**: `0x180` is `CODE_INDEX` (IMEM PIO port 0 control) and `0x184/0x188/0x18C` are its data / virt-addr / port-1 control registers. It pokes the IMEM write pointer to word 5, pushes 4 dwords of "descriptor" into IMEM at word 5 — **inside the secure code image** — and leaves the IMEM port registers holding scraps of the descriptor. | MEASURED (code) + SOURCE (envytools `rnndb/falcon.xml`) |

Conclusion: **delete variant B.** On this chip it is not a boot shape, it is IMEM corruption.
That is also why "both variants fail identically": A was already dead by defects 1–4, and B
additionally scribbled on the code.

### 2.2 The V2 descriptor, complete, for the record (Turing / GA100 only)

`struct flcn_bl_dmem_desc_v2` is **`__packed`** — 84 bytes, `0x54`, no padding anywhere.
This matters: a naturally-aligned C struct of the same field list is 96 bytes with 4 bytes of
padding after `ctx_dma`, and every field from `code_dma_base` on lands 4 bytes late.

| off | size | field | value for our FWSEC image |
|---|---|---|---|
| `0x00` | 16 | `reserved[4]` | 0 |
| `0x10` | 16 | `signature[4]` | 0 (no secure code at DMEM offset 0) |
| `0x20` | 4 | `ctx_dma` | **4** = `FALCON_DMAIDX_PHYS_SYS_NCOH` (the enum is 0,1,2,3,4 and then 6; **5 does not exist**) |
| `0x24` | 8 | `code_dma_base` (u64, **unaligned on purpose**) | phys of the staged image (256 B aligned) |
| `0x2C` | 4 | `non_sec_code_off` | 0 |
| `0x30` | 4 | `non_sec_code_size` | 0 |
| `0x34` | 4 | `sec_code_off` | `IMEMPhysBase` = 0 |
| `0x38` | 4 | `sec_code_size` | `0xF200` |
| `0x3C` | 4 | `code_entry_point` | 0 |
| `0x40` | 8 | `data_dma_base` (u64) | phys + `0xF200` |
| `0x48` | 4 | `data_size` | `0xD80` |
| `0x4C` | 4 | `argc` | 0 |
| `0x50` | 4 | `argv` | 0 |

Where it would be written, if it were used: **PIO to DMEM offset 0, port 0** —
`DATA_INDEX` (`0x1C0`) ← `BIT(24) | 0`, then 21 dwords to `DATA` (`0x1C4`). Not `0x180`
(IMEM port), and not via any "descriptor register" — there is no such register; `0x1C0/0x1C4`
*are* the DMEM port. One wrinkle worth knowing: the address field in `0x180`/`0x1C0` is bits
15:2 with `shr=2`, i.e. a **word** index (envytools), while nouveau writes the byte offset;
the two agree only because every falcon load in the tree uses base 0.

### 2.3 The three DMEM structures FWSEC *does* need — read off this card's ROM

All offsets are into the DMEM image, i.e. `image + 0xF200` … `image + 0x10280` of the
65408-byte payload at flash `0x44168`. **Patch these in the staging buffer before the load**,
exactly as both stacks do. Values below are from
`docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.gz` — no guessing left in this step.

| what | offset in the DMEM image | image offset | contents | tag |
|---|---|---|---|---|
| appif header | `+0x1C` (= `InterfaceOffset`) | `0xF21C` | `01 04 08 02` → ver 1, entry size 4, entry len 8, **cnt 2** | MEASURED |
| appif entry 0 | `+0x20` | `0xF220` | `id = 4` (DMEMMAPPER), `dmem_base = 0xAE0` | MEASURED |
| appif entry 1 | `+0x28` | `0xF228` | `id = 5`, `dmem_base = 0xD2C` | MEASURED |
| DMEMMAPPER v3 block | `+0xAE0` | `0xFCE0` | sig `0x50414D44` = bytes `44 4D 41 50`, i.e. "DMAP", ver 3, size `0x40` | MEASURED |
| ↳ `cmd_in_buffer_offset` | block `+0x08` | `0xFCE8` | field says `0xD40`, size `0x40`; the buffer itself is at image `0xFF40` | MEASURED |
| ↳ `init_cmd` | block `+0x2C` | `0xFD0C` | 0 → **must become `0x15`** (FRTS) | MEASURED / SOURCE |
| PKC signature slot | `+0xB24`, 384 B | `0xFD24` | **all zero**, must be filled | MEASURED |

The middle column is what a driver needs: the descriptor's `InterfaceOffset` (`0x1C`) indexes
the DMEM image, and every structure is reached by chaining `dmem_base` /
`cmd_in_buffer_offset` from there. `0xB24 + 384 = 0xCA4 < 0xD80`, so the signature patch stays
inside the DMEM load region and travels with the DMA.

The FRTS command itself goes at `cmd_in_buffer_offset`, 44 bytes, in this exact field order
(both stacks agree on every offset, including `hdr` = *struct size*, not a byte count of
anything else):

| off | field | value |
|---|---|---|
| `+0x00` | `read_vbios.ver` | 1 |
| `+0x04` | `read_vbios.hdr` | 24 |
| `+0x08` | `read_vbios.addr` (u64) | 0 |
| `+0x10` | `read_vbios.size` | 0 |
| `+0x14` | `read_vbios.flags` | 2 |
| `+0x18` | `frts_region.ver` | 1 |
| `+0x1C` | `frts_region.hdr` | 20 |
| `+0x20` | `frts_region.addr` | `frtsOffset >> 12` (4 KiB pages) = `0x5FE300` |
| `+0x24` | `frts_region.size` | `0x100` (1 MiB in 4 KiB pages) |
| `+0x28` | `frts_region.type` | 2 (FB) |

Total 44 ≤ the 64-byte buffer at `0xD40`, so it fits with 20 bytes to spare.

Our driver currently `(void)`-casts `frts_off` and `frts_size` at the top of
`nv_gsp_fwsec_launch()` and patches neither structure — even though the caller already
computes `frts_offset >> 12` (the page value this struct wants) and hands it over. FRTS is the
entire *purpose* of the launch; without `init_cmd` the ucode has nothing to do, and its exit
path is whatever an uninitialised command yields.

---

## 3. The GSP Boot-ROM parameter registers

The parent's four addresses are **correct**. They are the GSP falcon's *second-pri* window,
`NV_FALCON2_GSP_BASE = 0x111000`, which is the falcon base `0x110000` plus nouveau's
`addr2 = 0x1000`. So `0x111180 = 0x180` in that window, exactly as for SEC2 at `0x841180`.
The values are what was wrong.

| register | address | value | why | tag |
|---|---|---|---|---|
| `PARAADDR(0)` | `0x111210` | **`0xB24`** = the descriptor's `PKCDataOffset` | DMEM offset at which the Boot ROM will look for the RSA signature it must verify. `0x10` (the booter's value) makes it verify 0x10 — where the appif header lives. | SOURCE |
| `ENGIDMASK` | `0x11119C` | **`0x400`** = descriptor `EngineIdMask` | selects the engine class whose fused key version applies; 0x400 is the heavy-secure falcon class. `1` is the SEC2 booter's value. | SOURCE |
| `CURR_UCODE_ID` | `0x111198` | **`9`** = descriptor `UcodeId` | selects the fused key version at `0x8241C0 + (id-1)*4`. 9 is what the VBIOS says and what `nv_vbios_probe` already parses. | MEASURED + SOURCE |
| `MOD_SEL` | `0x111180` | **`1`** = `ALGO_RSA3K` | signature algorithm selector. 1 is correct. | SOURCE (`dev_falcon_second_pri.h`) |

Order rules, both stacks: written **after** the image load and **after** the reset, immediately
before `BOOTVEC`/mailbox/START, once per launch. A reset pulse in between would clear them
(the register block is inside the falcon, not sticky) — INFERRED, but both stacks sequence it
this way and it costs nothing to keep.

Also required at the same point: **`BOOTVEC` (`0x104`) = `IMEMVirtBase` = 0**. Our driver
writes 0, which is right, but it writes it once, before the (spurious) start probe, not per
launch.

---

## 4. The ordered launch sequence

`G` = `0x110000` (GSP falcon), `B` = `0x111000` (BROM window).

### 4.1 Preconditions — each one has bitten or can bite

| # | precondition | check | tag |
|---|---|---|---|
| A | **Per-run readability map.** Sample `G+0x000…G+0x13FF` and `B+0x000…B+0xFFF`, 64 bytes apart, and log the raw values *in the same run as the launch*. The existing map covers only `G+0x000…G+0x0FFF` (64 blocks) and was taken in a different boot; the `0x111xxx` window has **never** been sampled. | log | MEASURED (the gap) |
| B | **Do not trust the block map for a specific register.** In one and the same log, `0x110100` read `0x10` while `0x11010C` read `0xBADF5620` — both inside the *same* 64-byte block. The fence is finer than 64 B; treat every read as "real or fence word" and branch on that. | log | MEASURED |
| C | **Staging buffer: 256-byte aligned, size `ALIGN_UP(StoredSize,256)` = `0xFF80`, uncached or cache-flushed after the copy.** The vendor allocates it `NV_MEMORY_UNCACHED`; the DMA and the Boot ROM both read it as memory. A dirty cache line here reproduces the exact symptom under discussion. | log phys, alignment, high bits | SOURCE |
| D | **Physical address reachable by the DMA.** `DMATRFBASE` holds `phys>>8` (40 bits); `0x128` holds the rest. Log both, and log whether phys is above 4 GiB. | log | SOURCE |
| E | **The GSP must be in falcon mode, not RISC-V mode, during FWSEC.** `ga102_flcn_select` *clears* `0x668` bit 4 before the reset that precedes FWSEC; `0x111668 ← 0x111` happens only *after* FWSEC. If a previous stage left `0x111` in place, FWSEC is being started on the wrong core. | read `B+0x668`; if fenced, write 0 and continue | SOURCE (nouveau) |
| F | No other falcon load is in flight; `0x118` bit 0 (FULL) clear. | read | SOURCE |

### 4.2 Steps

| # | action | register / value | observable that proves it | tag |
|---|---|---|---|---|
| 1 | Reset — pre-wait | read `G+0x0F4`; wait ≤150 µs for bit 31 | raw `0x0F4` logged. nouveau only *warns* if it never sets, so a 0 here is a signal, not a gate | SOURCE |
| 2 | Reset — disable | `G+0x048 &= ~3`; `G+0x014 = 0xffffffff` | — | SOURCE |
| 3 | Reset — engine pulse | `G+0x3C0 = 1`; ≥10 µs; `G+0x3C0 = 0` | — | SOURCE |
| 4 | Reset — scrub done | poll `G+0x0F4` bit 12 = 0, ≤20 ms | bit 12 clears. **Bit 31 must not be reported as "reset done" — our last run printed `reset done=1` from a hard-coded default while `0x0F4` read `0x47B7` (bit 31 = 0)** | MEASURED |
| 5 | DMA may address sysmem with no context | `G+0x624` bit 7 = 1; `G+0x10C = 0` | none (read-fenced here); log raw | SOURCE |
| 6 | Route the DMA port | `G+0x600 = 0x5` (TRANSCFG for ctx 0: port valid + physical/non-coherent sysmem) | none (read-fenced); log raw | SOURCE |
| 7 | **Patch the DMEM image in the buffer** (before any transfer) | `init_cmd = 0x15` at DMEM `+0xB0C` (image `0xFD0C`); FRTS cmd at DMEM `+0xD40` (image `0xFF40`); 384 B signature at DMEM `+0xB24` (image `0xFD24`) | a hexdump of the 3 patched ranges, pre-DMA | SOURCE + MEASURED offsets |
| 8 | Load IMEM | 0xF200 B in 256 B blocks: `0x110 = (phys+off)>>8`, `0x128 = high`, `0x114 = off` (**destination advances**), `0x11C = off`, `0x118 = 6<<8 \| 1<<4 \| 1<<2` (SIZE=256, IMEM, SEC=1); poll `0x118` **bit 1** | per block, `0x118` bit 1 sets; after the last block `0x114` reads `0xF100` | SOURCE |
| 9 | Load DMEM | 0xD80 B, same but `0x114 = off`, no IMEM bit, SEC=0 | same | SOURCE |
| 10 | BROM params | `B+0x210 = 0xB24`, `B+0x19C = 0x400`, `B+0x198 = 9`, `B+0x180 = 1` | read back if the window answers; otherwise judged by behaviour (see D4) | SOURCE |
| 11 | Boot vector | `G+0x104 = 0` | read back | SOURCE |
| 12 | Prefill the verdict channels | `G+0x040 = 0xFEEDFACE`, `G+0x044 = 0xFEEDFACE`, **`0x1438 = 0xBEEF0000`** | read back `0x1438` — a scratch register, so the sentinel should stick (INFERRED, cheap) | INFERRED |
| 13 | Start | `G+0x100 = 2` (STARTCPU, bit 1) | sample `0x100` at +1 µs and at +10 ms: still `0x10` both times = start refused; clear at 1 µs then `0x10` = it ran and died | SOURCE |
| 14 | Wait for halt | poll `G+0x100` bit 4, ≤2 s | — | SOURCE |
| 15 | Verdict | `0x1438 >> 16` (FRTS error, 0 = ok), `G+0x040`, **`0x840100 != 0xBADF5620`** | `0x840100` is the *effect* of a successful FRTS: it drops the privilege level. This is the only gate that does not depend on a fenced read | SOURCE + MEASURED (fence word) |
| 16 | Second reset, then RISC-V | pulse `0x3C0` as in 2–4; then `B+0x668 = 0x111` | read back `0x668 = 0x111` | SOURCE |

Three notes that cost nothing and save a run:

- **Step 5/6 have no observable on this card.** That is not a reason to skip them; it is the
  reason step 8's `0x114` read-back and the PIO fallback (D1 below) exist.
- **The "start probe" that writes `0x100 = 2` before the params are in place must go.** It is
  a real start of a core with `BOOTVEC = 0`, no BROM params and no signature. Whatever state
  it leaves (halted, mid-BROM, error latched) is then the state the real launch inherits.
- **A second reset is required** after FWSEC, before the RISC-V switch. Both stacks do it;
  neither re-reads the BROM params afterwards.

---

## 5. Differential diagnosis: "START taken, mailbox untouched"

Read this table top to bottom — the rows are ordered by cost, and D13 is free.

| # | candidate cause | cheapest separating experiment | observation if this is the cause | next |
|---|---|---|---|---|
| **D13** | **The verdict channel is wrong, and nothing ever failed** | Prefill `0x1438` hi16 with `0xBEEF`, relaunch, then read `0x1438` and `0x840100`. Two reads plus one write | `0x1438` changes from `0xBEEF…` (FWSEC wrote its FRTS status) and/or `0x840100 != 0xBADF5620` | the premise collapses: FWSEC ran. Go straight to the booter |
| **D1** | **The image never reached the GSP** (defects 1–2) | read `G+0x114` and `G+0x118` right after the last IMEM block; if they answer (not the fence word) they must show `0xF100` and `…\|0x10` | `0x114` reads 0 or the fence word — no DMA was ever issued to this engine | fix `dst`/base/size, or switch to PIO (below) |
| **D2** | **No signature in DMEM at `0xB24`** (defect 3) | patch 384 B from `desc+0x2C+idx*0x180` into the buffer at `+0xFD24`, relaunch | `0x1438` hi16 moves off the sentinel (to 0 or to an error code) — i.e. the core got past the Boot ROM | if an error code: retry with `idx = 1` (the debug signature). `idx` comes from fuse version `0x8241C0 + 8*4` ANDed with `SignatureVersions` = 3 |
| **D3** | **BROM parameter values wrong** (defect 4) | they are wrong: run with `0xB24`/`0x400`/`9`/`1` | the launch gets past the halt it currently reaches | combine with D2: one relaunch fixes both |
| **D4** | **BROM parameters unwritable** (the `0x111xxx` block is write-fenced) | write `B+0x180 = 1`, read it back in a run where `B` was first mapped; also read `B+0x198` | read-back is the *written value* = it landed; fence word = no information; a real value ≠ written = **dropped** | if dropped: FWSEC cannot validate on this path at all → escalate to a dump, not another launch |
| **D5** | **Reset never completed** | log `0x0F4` bit 31 and bit 12 immediately after the pulse and again after 20 ms | bit 31 stays 0 and/or bit 12 settles 0 but bit 31 never sets → the core is not in a startable reset state | try the pulse without `0x014 = ~0`; if the window goes fully fenced, note that the reset costs the read path (MEASURED last time) |
| **D6** | **STARTCPU refused** (`HALTED` never clears) | sample `0x100` at +1 µs and +10 ms | `0x10` at both samples | precedence: fix E (`0x668`), D5, then retry. A refused start is not a ucode verdict |
| **D7** | **Core started and halted instantly** | same sampling as D6, plus `TRACEPC`: read `0x240` (SCTL), `0x148` (index), and walk `0x14C` per stack frame | `0x100` clears then re-sets; PC frames land in the Boot ROM range (high IMEM), not at 0 | if frames are in the BROM: signature/params (D2/D3). If at 0: the jump never happened |
| **D8** | **SEC bit wrong on IMEM** | flip IMEM `SEC` from 1 to 0 (one constant) | different halt signature, or an error where there was silence | low prior: both stacks load FWSEC IMEM with SEC=1. If SEC=0 makes it worse, that confirms 1 |
| **D9** | **GSP left in RISC-V mode** (`0x111668 = 0x111` set earlier) | read `B+0x668`; write 0 before the launch | the launch now behaves differently at all | keep the switch strictly after FWSEC (step 16) |
| **D10** | **FRTS command never patched** (defect: `frts_off` unused) | patch it, relaunch | `0x1438` hi16 becomes 0 with a `0x840100` that is no longer the fence word | does not explain "mailbox untouched", but it blocks the only success that matters |
| **D11** | **Needs a preceding step (FWSEC-SB)** | none — don't run it | — | ruled out: SB is the unload path in both stacks (vendor `kgspExecuteFwsecSb_HAL` at ~2582 of `kernel_gsp.c`; nouveau `tu102_gsp_fini`). Running it would only cost a launch |
| **D12** | **Staging buffer cacheable or misaligned** | align to 256 B, `clflush`/use an uncached mapping after the copy | behaviour changes with no other change | do it unconditionally (precondition C) |

### 5.1 If D1/D4 point at the transport, stop using DMA

The DMEM PIO round-trip is already **proven on this card** (`[NV-PROBE] verdict=WRITES LAND`,
`nv_selftest` writes a pattern through port 0 at `0x1C0/0x1C4` and reads it back). Loading
`0xF200 + 0xD80` bytes by PIO is ~16.4 k word writes — milliseconds — and it removes four
variables at once: DMA reachability, `CTXDMA`, TRANSCFG type, and cache coherence. It is the
single highest-information experiment available here, and it uses a register pair this driver
has already proved.

Two details if you take that route: IMEM PIO needs the `SECRET` bit (bit 28) on `0x180` and a
`CODE_VIRT_ADDR` (`0x188`) write per 256-byte block — our generic `flcn_pio()` helper sets
neither. DMEM PIO needs neither (`0x1C0` has no secret bit, and DMEM is loaded non-secure).

---

## 6. Contradictions in `docs/nv-pmu-acr-contract.md` §3.3

Each of these is a place where the doc sent the implementation somewhere the hardware does not
go. Fix them in the doc, not just in the code.

| § | doc says | what the sources say | tag |
|---|---|---|---|
| 3.3 step 6 | "write the DMEM boot descriptor … `tu102_gsp_fwsec_load_bld` … pushed by PIO to DMEM offset 0; and `0x600 + ctx*4 ← 5`" | That function belongs to `tu102_gsp_fwsec` (Turing), whose `load = gm200_flcn_fw_load` takes the `fw->boot` branch only when a separate `acr/bl` blob was loaded. GA102's `ga102_gsp_fwsec` has no `load_bld`, and the V3 path never sets `fw->boot`. **GA102 writes no descriptor.** The step is Turing/GA100-only and must be marked as such | SOURCE |
| 3.3 step 3 | "`0x600` ← `0x00000005` (ctx 0, TYPE 5 = SYSRAM, port valid)" | `0x600` is the per-context FBIF `TRANSCFG(ctx)` register (`0x600 + 4*ctx`). The context is the *register index*, not a field, and `5` is a value nouveau composes (port valid + type bits), not "TYPE 5 = SYSRAM". Neither stack's field encodings for it are published | SOURCE |
| 3.3 step 10 | "`0x1438`: high 16 = FRTS status, **low 16 = SB status**" | SB is a different scratch word: `PBUS_VBIOS_SCRATCH(0x15)` = **`0x1454`**, low 16 bits. High 16 of `0x1438` is right. §9's own source list already shows this — `nvkm_gsp_fwsec_sb` reads `0x1400 + 0x15*4`, not `0x1438` | SOURCE |
| 3.3 step 7 / §4 | "prefill mailbox … `0x040` ← 0 (FRTS: expected success value)"; "**`MAILBOX0 == 0` is the ground truth for both launches**" | For FWSEC the vendor passes `NULL` for both mailboxes and takes its verdict from `0x1438` hi16 and the WPR2 pair. nouveau writes 0 and checks 0, which cannot distinguish the two cases. MAILBOX0 is the ground truth for the **booter on SEC2**; for FWSEC it is an optional courtesy channel that the ucode may not use at all | SOURCE |
| 3.3 step 1/§4 row 4 | "`0x0F4` bit 31 set … `reset done`" | Bit 31 is the *pre-reset readiness* handshake read before the pulse; nouveau only warns if it never sets, and our post-pulse value was `0x47B7` (bit 31 = 0). "Reset done" cannot be inferred from bit 12 alone, and last run's `reset done=1` came from a hard-coded default, not a read | MEASURED |
| 3.4 / §5.8 | "the fence is per block rather than per engine" (64-byte blocks) | Our own log contradicts it: `0x110100` read `0x10` while `0x11010C` read `0xBADF5620`, in the same 64-byte block, in the same run. Fence granularity is finer than the map's resolution, and the map was taken in a different boot | MEASURED |
| 3.4 | SEC2 expectations after FRTS (`0x840100 = 0x10`) | Not contradicted, but the doc omits the one gate that matters and is measurable: **`0x840100 != 0xBADF5620`** as the effect of a successful FRTS. It should be stated as *the* FWSEC pass criterion on this card | SOURCE |
| §5 "Does the engine-reset pulse lift it?" | open | Still open, and now cheaper: the privilege drop is caused by FRTS, not by the pulse, so the pulse experiment cannot settle it. Prefer D13 | INFERRED |

Unchanged and confirmed by this pass: the FRTS struct layout and `>>12` units (§3.2), the
`0x100/0x104/0x118/0x11C/0x128` offset map (§3.3 steps 4–5, including the 256-byte block size
and `0x118` bit 1 as the completion bit), the second reset before the RISC-V switch (§3.3
step 11), and SB being teardown-only (§5).

---

## 7. Not established

| item | state |
|---|---|
| whether `0x1438` is writable (the D13 sentinel) | INFERRED yes — it is a PBUS scratch dword and it reads as a real 0, not a fence word |
| whether the `0x111000..0x111FFF` window is readable or writable | never sampled. The existing block map stops at `0x110FFF` |
| whether a write-fenced block silently drops writes | unknown. Writes to `0x110040` land; nothing proves the same for `0x110600`/`0x111180` |
| whether `0x111668 = 0x111` must be *cleared* rather than never set | nouveau clears it; the vendor's pre-FWSEC check is a capability check (`HWCFG2` bit 10, which reads 1 in our `0x47B7`), not a mode check. INFERRED, and the two readings agree on "0x111 after FWSEC, not before" |
| whether `0x084 (FALCON_RM)` = `PMC_BOOT_0` matters (nouveau's `gm200_flcn_enable` writes it) | not established; likely irrelevant to FWSEC, and it is one write if it turns out not to be |
| the exact field encoding of `TRANSCFG` (0x600) | unpublished in the open headers; only two working values are known (nouveau's `0x5`, the vendor's COHERENT_SYSMEM + PHYSICAL pair) |
| whether IMEM PIO needs `SECRET` when the load is DMA'd instead | moot for DMA; required for the PIO fallback (both stacks' PIO init sets `BIT(28)` when secure) |
| the numeric FWSEC error codes behind `0x1438` hi16 | unpublished. Log them, do not interpret them |

---

## 8. Sources

NVIDIA `open-gpu-kernel-modules` 535.113.01:

- `src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_falcon_ga102.c` —
  `kgspExecuteHsFalcon_GA102`: TRANSCFG, IMEM/DMEM DMA (`srcPhysAddr = base + codeOffset -
  imemVa`), the four BROM writes, `BOOTVEC = imemVa`, mailboxes, `kflcnStartCpu_HAL`,
  `kflcnWaitForHalt_HAL`.
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c` —
  `s_executeFwsec_TU102`: FRTS/SB command structs, `hsSigDmemAddr` patch, `0x1438` hi16 and
  `0x1454` lo16, WPR2 pair read-back; `kgspExecuteHsFalcon_HAL(..., NULL, NULL)`.
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c` —
  `s_vbiosNewFlcnUcodeFromDesc`: `dataOffset = imemSize`, `hsSigDmemAddr = PKCDataOffset`,
  `sigSize = RSA3K`, `signaturesTotalSize = descSize - 44`, `NV_MEMORY_UNCACHED`, 256 B align.
- `src/nvidia/src/kernel/gpu/gsp/arch/ada/kernel_gsp_ad102.c`,
  `.../arch/ampere/kernel_gsp_ga102.c` — `kgspBootstrapRiscvOSEarly_GA102` (reset → FRTS →
  `_kgspResetIntoRiscv`).
- `src/common/inc/swref/published/ampere/ga102/dev_falcon_v4.h` — CPUCTL, BOOTVEC, DMATRF*,
  `SEC 3:2`, `IMEM 4`, `SIZE 10:8`, `SIZE_256B = 6`.
- `src/common/inc/swref/published/ampere/ga102/dev_falcon_second_pri.h` —
  `NV_FALCON2_GSP_BASE 0x00111000`, MOD_SEL 0x180 (`ALGO_RSA3K = 1`), 0x198, 0x19C,
  `PARAADDR(i) 0x210+4i`.
- `src/common/inc/swref/published/turing/tu102/dev_bus.h` — `NV_PBUS_VBIOS_SCRATCH(i) =
  0x1400 + i*4`.
- `src/nvidia/arch/nvalloc/common/inc/rmflcnbl.h`, `flcnifcmn.h` — the pre-V2 `RM_FLCN_`
  descriptor (u32 bases, plus `argc/argv`), which is where the "11 dwords" shape comes from.

nouveau (Linux tree):

- `nvkm/subdev/gsp/fwsec.c` — `nvkm_gsp_fwsec_v3` (`imem_base_img = 0`, `dmem_base_img =
  IMEMLoadSize`, `dmem_size = ALIGN(DMEMLoadSize,256)`, `dmem_sign = PKCDataOffset`, signature
  patch from `desc+0x2c`), `_frts` (`0x1438` >> 16), `_sb` (`0x1454` low 16), `_patch`.
- `nvkm/subdev/gsp/ga102.c` — `ga102_gsp_fwsec` (no `load_bld`), `ga102_flcn_fw_boot` (the
  four `addr2` writes), `ga102_flcn_fw_load` (0x624 bit 7, 0x10C = 0, 0x600, `sec=true` for
  IMEM and `false` for DMEM), `ga102_gsp_reset` (`0x1668 ← 0x111`), `ga102_gsp_flcn.addr2 =
  0x1000`.
- `nvkm/subdev/gsp/tu102.c` — `tu102_gsp_fwsec_load_bld` (the only `bl_dmem_desc_v2` user, and
  only on the PIO path), `tu102_gsp_oneinit` (FRTS then reset).
- `nvkm/falcon/ga102.c` — `ga102_flcn_dma_init` (`(ilog2(len)-2)<<8`, IMEM `0x10`, sec
  `0x04`), `ga102_flcn_dma_xfer`, `ga102_flcn_dma_done` (`0x118` bit 1),
  `ga102_flcn_select` (`addr2+0x668` bit 4 cleared), `ga102_flcn_reset_prep` (0x0F4 bit 31).
- `nvkm/falcon/gm200.c` — `gm200_flcn_fw_boot` (mailbox prefill, `0x104`, `0x100 = 2`, 2 s
  halt), `gm200_flcn_disable`/`enable`.
- `include/nvfw/flcn.h` — `struct flcn_bl_dmem_desc_v2` `__packed`.

envytools: `rnndb/falcon.xml` (CODE_INDEX/DATA_INDEX `shr=2`, SECRET bit 28,
`CODE_VIRT_ADDR`, `XFER_STATUS` PENDING bit 1), `docs/hw/falcon/*`.

Local: `docs/nv-vbios-map.md` §6 (descriptor bytes), `docs/nvidia-driver.md` §5.8/5.9 (the
`hwcfg2=0x47B7` and `624/10c/600 = 0xBADF5620` rows), `docs/nv-gsp-wpr-contract.md` §3.2
(FRTS address), and the ROM dump under `docs/nv-vbios-evidence/` (the DMEM appif offsets in
§2.3 were recomputed from it for this document).
