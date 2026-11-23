# The GSP WPR2 / bootstrap contract on AD102 (RTX 4090, chipset 0x192, firmware 535.113.01)

Status: **contract document, not code.** Written 2026-11-23 on branch `nv/lane-wpr`.
Audience: whoever writes `nv_gsp_boot()` stages 3..8. Everything here is meant to be
implementable without reading another driver.

Nothing in this document was copied from a driver. Sources were read, the *contract*
was extracted, and it is stated here in tables and prose. Register names and bit names
are quoted because they are the wire format; behaviour is paraphrased.

---

## 0. Provenance tags, and how to read them

| tag | means |
|---|---|
| **MEASURED** | read off the target card by `docs/nv-gsp-reference.md` (NVIDIA 580.178.04 driving the same 4090, pre-init and post-init dumps). The offset is quoted so you can re-read it. |
| **MEASURED†** | computed here with our own parser (`nv_fw`) from the pinned 535.113.01 blobs in `third_party/nvfw/ad102/gsp/` and `.../ga102/gsp/`. It is a property of the file, not of the card, and it is reproducible with `sha256sum` on those bytes. |
| **SOURCE** | a named public file/function was read. Full list in §9. |
| **INFERRED** | my reasoning. Each one carries the observation that would falsify it. |

Two standing rules for this doc:

1. **Where a source and the measurement disagree, the measurement wins** and §6 says so
   explicitly. This card has already killed four source-shaped assumptions.
2. **"Not established" is a valid answer.** A wrong write into the write-protected region
   can wedge the card for the next boot, and r1's GPU has to stay usable. Every place
   where the honest answer is "we do not know" is marked that way, not filled in.

---

## 1. The contract in one page

Facts that change the shape of the plan, before any table:

| # | fact | tag |
|---|---|---|
| 1 | On Turing→Ada the **host does not program WPR2 at all**. The `booter_load` ucode — running on **SEC2**, not on the GSP — builds WPR2, loads the bootloader and the RM image into it, locks it, and starts the GSP. | SOURCE (NVIDIA `kernel_gsp.c`, comment above the booter allocation; and no write to `NV_PFB_PRI_MMU_WPR2_ADDR_*` exists anywhere in NVIDIA's source tree — only reads) |
| 2 | Consequently the host's whole job is: **compute a byte-exact FB layout, fill a 256-byte metadata block in system memory, point SEC2's mailboxes at it, and start SEC2.** | SOURCE (`tu102_gsp_wpr_meta_init`, `kgspExecuteBooterLoad_TU102`) |
| 3 | The RM image and the bootloader are **never DMA'd into VRAM by the host**. They stay in system memory; the meta describes them by *system physical address*, the RM image additionally through a **radix3 page-table tree**. The booter copies them into WPR2. | SOURCE (`kgspPopulateWprMeta_TU102`, `sysmemAddrOfRadix3Elf`) |
| 4 | `0x1FA824/0x1FA828` **are** `NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI` by NVIDIA's own register headers, and they are a **(start, end) pair**, both in 4 KiB units — not a 64-bit address split low/high. The values this card returns decode to a start above the end of VRAM and an end of 0, so they are not readable live words from an unprivileged BAR0 read. The cause is measurable: the read-protection bit at `0x1FA7C8` reads **set**, and RM reads the same words from a privileged context. | SOURCE + MEASURED, reasoning in §2.4 |
| 5 | There is **no separate WPR2 size register**: the extent is the pair itself (`end − start`), and it is also carried in the metadata at `+0xA8` for the bootloader's benefit. | SOURCE |
| 6 | The successful-boot proof that needs no RPC is the booter's own return code: **SEC2 `MAILBOX0` must be 0** after it halts. | SOURCE (`s_executeBooterUcode_TU102`) |
| 7 | `0x001668` from the old plan is **GSP-block-relative**, not BAR0-absolute: the live word is `0x111668`, and it is `NV_PRISCV_RISCV_BCR_CTRL` — the "run RISC-V, fetch from boot ROM" switch. That is what the `0x111` mask write means. | SOURCE + MEASURED (`0x111668 = 0x00000001` post-init) |
| 8 | The host must **not** write the WPR2 pair when the booter is going to run: RM refuses to boot when its own test (`HI.VAL != 0`) says a WPR2 is already up, and treats "WPR2 still up after unload" as an error. On this card that test reads "not up" even when one is, so it cannot be relied on in either direction. | SOURCE (`_kgspBootGspRm`, `kgspIsWpr2Up_TU102`, `kgspExecuteBooterUnloadIfNeeded_TU102`) |

---

## 2. WPR2: who writes it, in what order, and how you know

### 2.1 The three registers that matter, with names

| BAR0 offset | NVIDIA name | access | field | notes |
|---|---|---|---|---|
| `0x1FA824` | `NV_PFB_PRI_MMU_WPR2_ADDR_LO` | RW 32-bit | `VAL` = bits **31:4**, alignment field says **12** | **region start**, 4 KiB units: `start = VAL << 12` |
| `0x1FA828` | `NV_PFB_PRI_MMU_WPR2_ADDR_HI` | RW 32-bit | `VAL` = bits **31:4**, alignment 12 | **region end**, *not* the high half: `end = (VAL << 12) + 128 KiB` |
| `0x1FA7C8` | `NV_PFB_PRI_MMU_LOCK_CFG_PRIV_LEVEL_MASK` | RW 32-bit | bit 0 = `READ_PROTECTION_LEVEL0` | when set, unprivileged reads of the gated PFB MMU words are not the live value |

**The pair is (base, end), not (low, high).** That is the single most misreadable
thing in this block. The decode comes from the way RM itself turns the two words into a
range: start from `LO.VAL << 12`, end from `(HI.VAL << 12) + NV_WPR_BLOCK_GRANULARITY`
(128 KiB), and an empty region is reported when the two come out equal or the end is 0
(SOURCE: `kern_bus_gh100.c`, `kbusCarveoutWprs_GH100`). It is also the only reading
under which RM's own liveness test — "`HI.VAL != 0` means WPR2 is up" (SOURCE:
`kgspIsWpr2Up_TU102`) — makes any sense: a region with no end is not a region.

Provenance for the names/fields: SOURCE `src/common/inc/swref/published/turing/tu102/dev_fb.h`
(WPR2 pair) and `.../ampere/ga100/dev_fb.h` (the same block's LOCK registers, which
carry the `__PRIV_LEVEL_MASK` pointers and `LOCK_CFG`'s `READ_PROTECTION_LEVEL0`).
There is no `ad102/dev_fb.h` in NVIDIA's tree — Ampere's `ga100` header is the one that
covers this block on Ada.

Reset value, which is the second most useful number here: `WPR2_ADDR_LO_VAL_INIT =
0x0fffffff` (SOURCE, tu102/gh100 headers). Under the decode above that is a *start* of
`0x0FFFFFF000` = 64 GiB − 4 KiB — i.e. **"unprogrammed" is an address above any real
FB**, not zero. Since the *end* word's init value is `0`, the documented "down" state is
`LO` = sentinel, `HI` = 0.

### 2.2 Who writes it — and therefore what HobbyOS should do

| step | actor | tag |
|---|---|---|
| compute the FB layout and write it into the 256-byte meta **in system memory** | host | SOURCE (`kgspPopulateWprMeta_TU102`) |
| put the meta address into SEC2 `MAILBOX0/1`, start SEC2 | host | SOURCE (`kgspExecuteBooterLoad_TU102`) |
| verify the meta, program PFB WPR2 from `gspFwWprStart` to `gspFwWprEnd`, lock it | **booter ucode on SEC2** | SOURCE (`kernel_gsp.c`: "Booter Load loads, verifies, and boots GSP-RM in WPR2"; `gsp_fw_wpr_meta.h`: "Verified, and locked in WPR2 by Booter") |
| copy the RM image from sysmem (radix3) and the bootloader into WPR2 | booter | SOURCE (same) |
| tear WPR2 down again at driver unload | **booter_unload ucode on SEC2** | SOURCE (`kgspExecuteBooterUnloadIfNeeded_TU102`, nouveau `tu102_gsp_booter_unload`) |
| write `0x1FA824/28` from the host | **nobody, on this generation** | SOURCE (no host write path exists in either driver) |

**So the contract for question 1 is a negative one, and that is the finding: the
correct WPR2 programming sequence on AD102/535 is "do not program it".** What the host
must get right is the *layout* (§3) and the *meta* (§3.3); the booter turns those into
register writes. The measurement agrees with the negative claim: the pair never shows a
value a host-written layout could explain, and on this card it cannot be read live at all
(§2.4).

If WPR2 does have to be touched by hand later (debug only), the shape of the write is:
confirm `0x1FA7C8` bit 0 is clear (else the write will not be honoured or will not read
back), write the **end** word (`HI` = `(end − 128 KiB) >> 12`), then the **start** word
(`LO` = `start >> 12`), then read both back. **Tag: INFERRED** — no source states the
write order or whether the start must be written last, and nothing here should be
attempted on r1's card without a recovery plan. Falsifier: a source that shows the
pair written in the other order, or a read-back that does not match after a successful
write.

### 2.3 Region shape, units and the AD102 numbers

| property | value | tag |
|---|---|---|
| address unit | 4 KiB in both words (`VAL` bits 31:4, alignment 12) | SOURCE |
| start word | `LO.VAL = start >> 12` | SOURCE |
| end word | `HI.VAL = (end − 128 KiB) >> 12`, because RM reconstructs `end = (VAL << 12) + 128 KiB` | SOURCE |
| size | **no size register.** Derivable as `end − start` from the pair, and carried explicitly as the meta field at `+0xA8` | SOURCE |
| end alignment | **128 KiB** (`WPR_ALIGNMENT`); computed as `ALIGN_DOWN(vgaWorkspaceOffset − wprEndMargin, 128K)` | SOURCE |
| start alignment | the meta block is rounded up to **1 MiB**, so the start lands 1 MiB-aligned | SOURCE |
| boot-bin offset alignment | 4 KiB | SOURCE |
| RM ELF offset alignment | 64 KiB | SOURCE |
| WPR heap offset/alignment | 1 MiB | SOURCE |
| this card, computed | start `0x5F8700000`, end `0x5FE400000`, size `0x5D00000` (93 MiB), under the "reserve PRAMIN size at the top" rule; `0x5FA500000`–`0x5FF300000` (78 MiB) under nouveau's "reserve 1 MiB" rule | MEASURED†/INFERRED, see §3.2 |
| what the pair *would* read if it were live and matched the layout | `LO = 0x005F8700`, `HI = 0x005FE3FE` (or `0x005FA500` / `0x005FF2FE` under nouveau's rule) | INFERRED from SOURCE |

That last row is the important, falsifiable prediction: **a live, layout-consistent WPR2
on this card reads `LO ≈ 0x5F87xx`–`0x5FA5xx` and `HI ≈ 0x5FE3xx`–`0x5FF2xx`.** The card
reads `0x1FFFFE00` / `0` (§2.4). The two cannot both be true.

Two more predicted waypoints from sources, both checkable in a during-boot capture:

| moment | expected `0x1FA824` | expected `0x1FA828` | source |
|---|---|---|---|
| after FWSEC-FRTS, before the booter | `frtsOffset >> 12` = `0x5FE300` (`0x5FF200` under nouveau's rule) | non-zero | SOURCE: the FRTS path *verifies* `LO.VAL == frtsOffset >> 12` and treats `HI.VAL == 0` as "no initialized WPR2 found" |
| after the booter, during GSP-RM's life | `gspFwWprStart >> 12` | `(gspFwWprEnd − 128K) >> 12` | INFERRED from the layout (the booter rebuilds the region to cover the whole WPR) |

Those two row pairs are the strongest available evidence that the *host-visible* meaning
of the pair is (start, end), and they also say WPR2 has **two different extents during
one boot**: first just the FRTS region, then the whole WPR.

### 2.4 Which registers are actually WPR2 on AD102 — the honest state of it

Required by the parent lane, because a read-only dump is cheap and a wrong conclusion
here is expensive.

**(a) Best identification from sources.** `0x1FA824/0x1FA828` are WPR2, by name, in
NVIDIA's published register headers (Turing and Hopper both; Ada has no `dev_fb.h`, and
Ampere's `ga100` header carries the same block's LOCK registers referring back to the
same `PRIV_LEVEL_MASK` address). Nouveau's r535 debug print of the FRTS path also calls
this pair "WPR2". That is as good as identification gets without a datasheet.

**(b) Why the measured values cannot be taken at face value.**

| observation | tag | consequence |
|---|---|---|
| `0x1FA824 = 0x1FFFFE00` → start = `0x1FFFFE0000` = **128 GiB − 2 MiB** | MEASURED | a start larger than the 24 GiB FB. Not a base for any region the booter could have built. |
| `0x1FA828 = 0` in **both** states | MEASURED | by RM's own decode, `end = 0` = *no WPR2 at all* — and this is the post-init dump, taken with GSP-RM running and Vulkan rendering. RM's own predicate (`HI.VAL != 0`) would call this card "WPR2 is not up". |
| the identical value `0x1FFFFE00` also appears at `0x1FA81C` (WPR1_ADDR_LO on Hopper) | MEASURED + SOURCE | two different regions cannot both start at the same non-zero address; at least one read is not live data |
| `0x1FA7C8 = 0x0045CB8F`, **bit 0 set** = READ_PROTECTION_LEVEL0 enabled | MEASURED + SOURCE | names the mechanism: these PFB MMU words are inside a read-protected group, and RM reads them from a privileged context |
| `0x1FA82C/0x1FA830` are `LOCK_ADDR_LO/HI` by name; measured `0x1FFFFFF0` / `0` decodes to a start of 128 GiB − 4 KiB and an end of 0 | MEASURED + SOURCE | same failure mode, same window: strong corroboration that the whole `0x1FA7C8..0x1FA840` group reads protected |
| `0x1FA804/08`, `0x1FA82C/30` change across init while `0x1FA824/28` do not | MEASURED | so *some* words in this window do reflect driver activity — the window is live, individual words are gated |

**Conclusion (INFERRED, high confidence):** the WPR2 pair is correctly identified, and
the (start, end) decode is established, but the words **cannot be used as a state oracle
from an unprivileged BAR0 read on this card** — the card returns a start above its own FB
and an end of 0 while a GSP-RM is demonstrably running. The read-protection bit being set
is the only explanation consistent with all six rows. Consequences:

- do not gate a boot decision on these two words (the old "nonzero hi means firmware is
  up" test was *right in principle* and useless in practice on this card);
- do not write them;
- do not "correct" the local docs to say the pair *is* the live WPR2 — say it is *named*
  WPR2 and that the reads are protected. The wording "the pair that moves across init"
  stays correct for `0x1FA804/08`.

**This answers open item 1a of `docs/nvidia-driver.md` §8** as far as sources can:
the WPR2 registers are `0x1FA824` (start) and `0x1FA828` (end); the reason they "never
move" is that the values cannot be read under the current protection; and
`0x1FA804/0x1FA808` remain unexplained, with §2.4(c) as the experiment that would
identify them. The parent's honest wording ("the pair that moves is not proven to be
WPR2") is therefore better than "the WPR2 live check", and both are better than the
original "nonzero hi means the firmware is up".

**(c) The read-only dump to settle it.** No writes, no VM changes, one pass. Sample
these, in this order, in **one** capture, and again in a tight loop around a
`modprobe nvidia` (that is the experiment `docs/nv-gsp-reference.md` §8 already says is
cheap):

| address | candidate identity | value that confirms | value that refutes |
|---|---|---|---|
| `0x1FA7C8` bit 0 | read-protection enable for the group | **1** ⇒ treat all rows below as unreadable; stop here | 0 ⇒ the rows below *are* live and the decode of `0x1FA824` must be re-derived |
| `0x1FA824` | WPR2 **start**, `start>>12` | `0x005F8700` or `0x005FA500` (the two layouts of §3.2), and `(VAL<<12) ==` the meta's `gspFwWprStart` read out of VRAM | anything that decodes outside `[0, fbSize)`; `0x1FFFFE00` again = still protected |
| `0x1FA828` | WPR2 **end**, `(end−128K)>>12` | `0x005FE3FE` or `0x005FF2FE`, and `(VAL<<12)+0x20000 ==` the meta's `gspFwWprEnd` | `0`; RM's own predicate then says "no WPR2", which cannot be true on a working card |
| `0x1FA81C` / `0x1FA820` | WPR1 base lo/hi (Hopper naming; identity on Ada unknown) | different value from `0x1FA824` while GSP-RM runs | equal value ⇒ the pair aliases, and neither is usable |
| `0x1FA804` / `0x1FA808` | **unknown.** "the pair that moves across init" | changes *during* a boot and returns to `0x041B3000`/`0x05CF3000` after the owner exits | never moves during a boot ⇒ it is a POST leftover, not a boot-programmed word |
| `0x1FA80C`, `0x1FA814`, `0x1FA818`, `0x1FA81C`, `0x1FA840` | unknown; `0x1FA814/18 = 0x08000000` in both states | — | — |
| `0x1FA82C` / `0x1FA830` | `MMU_LOCK_ADDR_LO/HI` by name | a value that decodes inside `[0, fbSize)` | as measured: above FB ⇒ protected |
| `0x1180F8` / `0x1180FC` | GC6 BSI secure scratch 14 / 15 — the booter's handoff words | a non-zero "stage/handoff" progression during boot | never written ⇒ the booter uses another channel on 535 |

Two hypotheses to test with the same dump, both currently **INFERRED and unproven**:

- H1: `0x1FA808 = 0x05CF3000` (≈ 97.3 MiB) pre-init is *near* the WPR heap size this
  firmware asks for (§3.2 computes 54–127 MiB depending on clamp), and `0x1FA82C =
  0x00100000` is exactly the 1 MiB non-WPR heap. If so, "the pair that moves" is a
  *heap* base/size pair, not WPR2. Falsifier: during-boot capture in which these words
  do not track the meta's `gspFwHeapOffset/Size` fields.
- H2: the `0x1FA804/08` pair is a POST/VBIOS leftover that the *driver* clears on
  unload, not a region descriptor. Falsifier: it moves while the driver is loading, or
  it does not return to the pre-init signature after a clean unload.

### 2.5 Teardown, reuse, and "which state am I in"

The states a card can be handed to us in, and what each demands:

| state | how you recognise it (all read-only) | what to do | tag |
|---|---|---|---|
| **fresh / POST** | `0x1FA824` reads `0x0FFFFFF0` and `0x1FA828` reads `0` (the documented pair of init values) *if* they are readable at all; `0x110100` (`UC_CTRL`) bit 4 set; `0x840000`-window fenced (`0xBADF5620`-class) | normal path: compute layout, meta, booter | SOURCE + INFERRED |
| **mid-boot, right after FWSEC-FRTS** | `0x1FA824` = `frtsOffset >> 12`, `0x1FA828` non-zero (RM's own post-FRTS check) | a boot is in progress somewhere else — do not start a second one | SOURCE |
| **someone else's GSP-RM is up** | `0x110108` = `0x80420100`, `0x11012C` = `0xCA101136`, `0x110100` = `0x10`; the `0x111000` window answers instead of being fenced | **do not boot.** RM refuses in this state; only a booter_unload (a ucode we would have to run on SEC2, with a *matching* meta) may tear it down | MEASURED + SOURCE |
| **after a driver exited cleanly** | the measured signature: `0x1FA804 = 0x041B3000`, `0x1FA808 = 0x05CF3000`, `0x1FA82C = 0x00100000`, `0x1FA830 = 0x05CF3FF0`, `0x1FA824 = 0x1FFFFE00`, `0x1FA828 = 0` | this is the **normal** start state for us — the region is already down; lay out and boot | MEASURED (both dumps agree, `docs/nvidia-driver.md` §5.5) |
| **unknown** | anything else | refuse, log the 12 words, and require an operator decision. Never "try it and see" | — |

Two hard rules that fall out of this:

- **Never write WPR2 to "reuse" it.** Reuse in this architecture means "the booter
  rebuilds it from our meta". If a stale region really is up, the only supported exit is
  booter_unload with the *same* layout that created it.
- **After our own failed attempt**, the expected observable is a non-zero SEC2
  `MAILBOX0` (the booter reports its own error), not a wedged card. If `MAILBOX0` is
  non-zero, log it, do not retry blindly, and treat the card as "may need a reset".

---

## 3. The layout inside the WPR region

### 3.1 What lives where (top of FB downwards)

NVIDIA's own layout routine documents this shape; the table is my restatement of it,
with the alignment rules it applies. **All addresses are VRAM byte offsets from the
base of FB**, not system physical addresses and not BAR1 offsets (they coincide 1:1 on
this card, see §3.4).

| region | size | alignment of its start | tag |
|---|---|---|---|
| VGA/PRAMIN workspace (not ours) | reserve at top | start = `fbSize − PRAMIN_size` (16 MiB) per NVIDIA; nouveau uses `fbSize − 1 MiB` | SOURCE |
| `gspFwWprEnd` | — | 128 KiB down from the top of the usable area, minus a "WPR end margin" (0 normally) | SOURCE |
| FRTS data (`frtsOffset`) | 1 MiB | immediately below `gspFwWprEnd` | SOURCE (`frtsSize` = 1 MiB for non-GA100) |
| boot binary: bootloader ("SK + BL") (`bootBinOffset`) | `sizeOfBootloader` = 32768 on this firmware | 4 KiB | SOURCE + MEASURED† |
| GSP-RM ELF image (`gspFwOffset` = `meta.gspFwOffset`) | `.fwimage` size = 38 043 648 | 64 KiB | SOURCE + MEASURED† |
| GSP-RM WPR heap (`gspFwHeapOffset`) | policy size, clamped by what is left | 1 MiB | SOURCE |
| **GspFwWprMeta copy** (`gspFwWprStart`) | 256 B used, 1 MiB reserved | 1 MiB | SOURCE |
| non-WPR heap (`nonWprHeapOffset` = `gspFwRsvdStart`) | 1 MiB | 1 MiB | SOURCE |
| … everything below `nonWprHeapOffset` is free for the driver, subject to the scrub rule | | | |

Rules that are easy to get wrong:

- `gspFwWprEnd` is **not** "top of FB": the VGA workspace and (if VBIOS locked it) the
  MMU_LOCK row-remapper region sit above it, and on GA100 the layout is clamped to the
  LOCK address. On AD102 the LOCK read is useless (§2.4), so the safe reading is "the
  VGA workspace rule alone".
- The **GSP firmware scrubs the last 256 MiB of FB** on boot. Nothing outside the WPR
  layout above may be allocated there until GSP-RM has finished scrubbing.
  SOURCE (comment above NVIDIA's layout routine).
- The meta block is *reserved as 1 MiB* even though the structure is 256 bytes, and the
  base is therefore 1 MiB-aligned. Do not "optimise" this to 128 KiB: the field is named
  `gspFwWprStart` and the 128 KiB rule applies to the **end** only.
- `nonWprHeapOffset == gspFwRsvdStart`, same value, two names. Both must be written.

### 3.2 The AD102 / 535.113.01 case, computed

Inputs: FB usable = **24564 MiB = 0x5FF400000** (that is what `nvidia-smi` reports as
total on the reference card; MEASURED), `.fwimage` = **0x2448000**, bootloader data =
**0x8000**, FRTS = **0x100000**.

| step | NVIDIA rule (reserve 16 MiB) | nouveau rule (reserve 1 MiB) |
|---|---|---|
| `gspFwWprEnd` | `0x5FE400000` | `0x5FF300000` |
| `frtsOffset` | `0x5FE300000` | `0x5FF200000` |
| `bootBinOffset` | `0x5FE2F8000` | `0x5FF1F8000` |
| `gspFwOffset` | `0x5FBEB0000` | `0x5FCDB0000` |
| WPR heap available after the above | 54 MiB | 39 MiB |
| `gspFwHeapOffset` | `0x5F8800000` | `0x5FA600000` |
| `gspFwWprStart` | **`0x5F8700000`** | **`0x5FA500000`** |
| `nonWprHeapOffset` | `0x5F8600000` | `0x5FA400000` |
| WPR2 size | **93 MiB** | 78 MiB |
| equivalent `WPR2_ADDR_LO` | `0x005F8700` | `0x005FA500` |

**Tag: MEASURED†-derived, with two INFERRED assumptions marked below.** The step-by-step
arithmetic follows from SOURCE (`kgspPopulateWprMeta_TU102`) exactly; what is inferred:

- the usable-FB figure (24564 MiB) is `nvidia-smi`'s total, not an RM-internal
  `kmemsysGetUsableFbSize` — falsifier: a boot in which `fbSize` computed from
  `PMC_BOOT_0`/VBIOS differs from 24564 MiB;
- the heap size is a **policy value clamped by available space**, not a constant:
  the documented formula (20 MiB OS carveout + 8 MiB base + 96 KiB/GB-of-FB + 96 MiB
  client alloc, floor 84 MB) yields 127 MiB, which does not fit in 54 MiB, so it is
  clamped. Falsifier: a GSP boot whose meta shows a `gspFwHeapSize` different from the
  available space — which would mean the clamp is not "use what is left".

The **difference between the two rules is exactly 15 MiB and it matters**: 16 MiB is the
size of PRAMIN (BAR0's window), and a WPR whose end sits inside the VGA workspace is a
corrupt layout the booter is entitled to reject. Prefer NVIDIA's rule (reserve the
PRAMIN size), and log both numbers so the discrepancy is visible if the booter complains.

### 3.3 The 256-byte metadata block, field by field

This **replaces the hypothesis in `src/include/nv_fw.h`**, which was wrong in shape (it
assumed `fb_size / wpr_start / wpr_size / region[3]`; there is no region array and no
WPR2 size field at all). Two things the old hypothesis got right: the 256-byte size and
magic `0xDC3AAE21371A60B3`, revision 1.

Authoritative offsets: SOURCE `src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta_v1.h`
(the V1 structure that "Booter (Turing through Ada)" consumes; the newer
`gsp_fw_wpr_meta.h` revision is *not* what 535 expects). Values in the right column are
this card + this firmware.

| off | size | field | value / source of value | tag |
|---|---|---|---|---|
| `0x00` | 8 | magic | `0xDC3AAE21371A60B3` | SOURCE |
| `0x08` | 8 | revision | `1` | SOURCE |
| `0x10` | 8 | `sysmemAddrOfRadix3Elf` | **system physical** address of the radix3 root that describes the RM image | SOURCE |
| `0x18` | 8 | `sizeOfRadix3Elf` | `0x2448000` (`.fwimage` length) | MEASURED† |
| `0x20` | 8 | `sysmemAddrOfBootloader` | **system physical** address of the bootloader's data region (the 0x8000 bytes at file offset 0x6C) | SOURCE + MEASURED† |
| `0x28` | 8 | `sizeOfBootloader` | `0x8000` | MEASURED† |
| `0x30` | 8 | `bootloaderCodeOffset` | `0x4800` (from the bootloader's own descriptor) | MEASURED† |
| `0x38` | 8 | `bootloaderDataOffset` | `0x800` | MEASURED† |
| `0x40` | 8 | `bootloaderManifestOffset` | `0` on this firmware | MEASURED† |
| `0x48` | 8 | `sysmemAddrOfSignature` | **system physical** address of the 4096-byte `.fwsignature_ad10x` section (copy it into contiguous frames; the section lives inside the ELF, so either place the whole ELF or copy the section) | SOURCE |
| `0x50` | 8 | `sizeOfSignature` | `0x1000` | MEASURED† |
| `0x58` | 8 | `gspFwRsvdStart` | = `nonWprHeapOffset` | SOURCE |
| `0x60` | 8 | `nonWprHeapOffset` | `0x5F8600000` (§3.2) | INFERRED layout |
| `0x68` | 8 | `nonWprHeapSize` | 1 MiB | SOURCE |
| `0x70` | 8 | **`gspFwWprStart`** | `0x5F8700000` (§3.2) — the WPR2 base the booter will program | INFERRED layout |
| `0x78` | 8 | `gspFwHeapOffset` | `0x5F8800000` | INFERRED layout |
| `0x80` | 8 | `gspFwHeapSize` | 54 MiB | INFERRED layout |
| `0x88` | 8 | `gspFwOffset` | `0x5FBEB0000` (RM image destination) | INFERRED layout |
| `0x90` | 8 | `bootBinOffset` | `0x5FE2F8000` (bootloader destination) | INFERRED layout |
| `0x98` | 8 | `frtsOffset` | `0x5FE300000` | INFERRED layout |
| `0xA0` | 8 | `frtsSize` | `0x100000` | SOURCE |
| `0xA8` | 8 | **`gspFwWprEnd`** | `0x5FE400000` — the only place the region's *end* exists | INFERRED layout |
| `0xB0` | 8 | `fbSize` | `0x5FF400000` | MEASURED |
| `0xB8` | 8 | `vgaWorkspaceOffset` | `0x5FE400000` | INFERRED layout |
| `0xC0` | 8 | `vgaWorkspaceSize` | `fbSize − vgaWorkspaceOffset` | SOURCE |
| `0xC8` | 8 | `bootCount` | `0` on first boot; the booter increments/uses it to decide whether to reload | SOURCE |
| `0xD0` | 16 | reserved | zero | SOURCE |
| `0xE0` | 8 | `sysmemAddrOfCrashReportQueue` | 0 if no CrashCat queue | SOURCE |
| `0xE8` | 4 | `sizeOfCrashReportQueue` | 0 | SOURCE |
| `0xEC` | 4 | reserved | 0 | SOURCE |
| `0xF0` | 1 | `gspFwHeapVfPartitionCount` | `0` (bare metal, no VF partitions) | SOURCE |
| `0xF1` | 1 | `flags` | `0` | SOURCE |
| `0xF2` | 2 | `pagingConfig` | `0` | SOURCE |
| `0xF4` | 4 | reserved | 0 | SOURCE |
| `0xF8` | 8 | **`verified`** | host writes `0`; **the booter writes `0xA0A0A0A0A0A0A0A0`** when it accepts the meta | SOURCE |

**Fields I could not establish** (and why it matters):

| question | why it is open | matters because |
|---|---|---|
| does the booter require `gspFwWprEnd`/`fbSize` to match its own idea of FB exactly, or does it clamp? | no source states the validation predicate; only that the booter "verifies" the meta | a mismatch is a boot failure with a non-zero mailbox and no other clue |
| is `frtsSize = 0x100000` a fixed constant or per-chip policy? | NVIDIA's routine calls a HAL (`kgspGetFrtsSize`) I did not read | a wrong FRTS size shifts `bootBinOffset` and everything above it |
| what exactly does `pagingConfig` do | it is a passthrough of a registry bitmask | a non-zero value may change how the booter maps the ELF |
| `flags` bit values beyond `IN_PM_TRANSITION` | only some flags are named | the recovery paths (ECC scan margin) use them |

**Read-back that proves the meta was accepted:** after the booter returns, read the
256-byte copy at `gspFwWprStart` through BAR1 and check magic + `verified ==
0xA0A0A0A0A0A0A0A0`. Tag: INFERRED (the field semantics are SOURCE; that the copy is at
`gspFwWprStart` and readable post-lock is my reading of "DMA'd to FB at the end of what
will be WPR2 … locked in WPR2 by Booter"). Falsifier: a boot where `verified` stays 0
while the RPC handshake succeeds.

### 3.4 Address spaces — the thing that silently kills a boot

| quantity | space | note |
|---|---|---|
| every `sysmemAddrOf*` in the meta | **system physical**, 64-bit | must be physically contiguous; the image is 38 MB, so the frame allocator has to produce one run |
| `radix3` tree | array of 64-bit PTEs, each a **system physical** page address, 4 KiB granularity, three levels | the booter walks it to DMA the ELF; non-contiguous pages are *not* supported in practice (the driver itself builds a trivially-linear tree) |
| `gspFwWprStart`, `gspFwOffset`, `bootBinOffset`, `frtsOffset`, `gspFwWprEnd`, heaps, `fbSize`, `vgaWorkspace*` | **VRAM byte offset from FB base** | not BAR1 offsets, not physical addresses; on this card they numerically coincide if BAR1 maps VRAM 1:1, but that is an assumption (§3.5) |
| SEC2 `MAILBOX0/1` | low/high 32 bits of a **system physical** address (the meta) | the booter dereferences it as a sysmem pointer |
| GSP `MAILBOX0/1` | low/high 32 bits of a **system physical** address (the libos args array, §4) | written *before* the booter runs |
| booter image source address | **system physical**, and the DMA base register only carries 40 bits of it: `DMATRFBASE` = bits 31:0 of `addr>>8`, `DMATRFBASE1` = bits 8:0 of `addr>>40` | a buffer above 1 TiB is unreachable; nouveau writes 0 to `DMATRFBASE1`, which silently breaks above 4 GiB if the buffer is high |

### 3.5 BAR1 assumptions to verify before writing anything

BAR1 on the reference card is **32 GiB** while VRAM is 24 GiB (MEASURED, `lspci -vvv`),
so the aperture is *larger* than the memory it maps. Three consequences, the first of
which is a blocker:

- **Our own layout lives near 24 GiB into the aperture** (`0x5F8700000` ≈ 24.4 GiB).
  `docs/nvidia-driver.md` §8 records that the driver currently maps **1 GiB** of BAR1.
  A 1 GiB window cannot address the WPR region at all, so *before* any step that touches
  VRAM (writing the meta copy, reading it back, verifying anything at `gspFwWprStart`)
  the window must be re-based to cover `[gspFwWprStart, gspFwWprEnd)`, or PRAMIN's 16 MiB
  window must be used with its own paging. **Which register re-bases the BAR1 window on
  AD102 is not established in this doc** — it needs its own lane, and it gates step 9 of
  §8.
- writing a VRAM offset through BAR1 only works if the mapping is 1:1 for the region we
  touch (all our offsets are inside the 24 GiB, so the *content* is addressable once the
  window covers it);
- a 32 GiB aperture over 24 GiB of memory means the top 8 GiB alias or fault — do not
  use "BAR1 size" as a proxy for VRAM size anywhere.

**Falsifier for the 1:1 assumption:** write a known pattern to BAR1 offset X inside the
rebased window, read it back through a second path, compare. Until then, treat
"BAR1 offset == VRAM offset" as INFERRED.

---

## 4. How the booter, the bootloader and the RM image are delivered

### 4.1 Which processor runs what — the correction that matters

The task framing ("how is the booter delivered to the GSP processor") has a surprising
answer on this generation: **the booter does not run on the GSP.** It runs on **SEC2**,
the security coprocessor, and it is SEC2 that programs WPR2 and starts the GSP.

| stage | runs on | loaded by | delivered as | tag |
|---|---|---|---|---|
| `booter_load` (55 928 B container, 0x7600 IMEM + 0x6000 DMEM image) | **SEC2** falcon/RISC-V | host | IMEM + DMEM **DMA from system memory**, plus BROM parameter registers | SOURCE |
| bootloader ("SK + BL", 32 768 B) | **GSP** RISC-V | **booter** | copied into VRAM at `bootBinOffset` from `sysmemAddrOfBootloader` | SOURCE |
| GSP-RM ELF (38 MB) | **GSP** RISC-V | **booter** | copied into VRAM at `gspFwOffset` via the **radix3** tree from system memory | SOURCE |
| GSP-RM proper | GSP RISC-V, then RPC server | bootloader | already in WPR2 | SOURCE |

So there is no "boot ROM reads a VRAM-resident image" step on the host side, and the
GSP's own boot ROM only runs *after* the booter has placed the bootloader in WPR2.

### 4.2 The exact SEC2 sequence (register offsets are SEC2-base relative)

SEC2's BAR0 window is `0x840000`, its FBIF window `0x840600`, and its RISC-V register
block is `+0x1000` (`0x841000` — by symmetry with the GSP, whose RISC-V block is
`0x110000 + 0x1000`; **the SEC2 RISC-V base itself is INFERRED**, the GSP one is SOURCE).
Order is load-bearing; each row is one write or one poll.

| # | register | offset | value / bit fields | why | tag |
|---|---|---|---|---|---|
| 1 | `NV_PFALCON_FALCON_CPUCTL` | `0x100` | read; require bit 4 (`HALTED`) or reset first | never load a running core | SOURCE |
| 2 | engine reset | — | assert the engine's reset, wait for memory scrubbing to finish (`HWCFG2` bit 12 clears) | the core comes up STOPPED | SOURCE |
| 3 | `FBIF_TRANSCFG(0)` | `0x840600` (the FBIF window's first register) | `TARGET = COHERENT_SYSMEM`, `MEM_TYPE = PHYSICAL` | the DMA fetches from sysmem, coherently | SOURCE |
| 4 | `DMATRFBASE` | `0x110` | `(src_phys >> 8)` low 32 bits | DMA source base, 256-byte granularity | SOURCE |
| 5 | `DMATRFBASE1` | `0x128` | `((src_phys >> 40) & 0x1FF)` | high bits; **0 only if the buffer is below 4 GiB** | SOURCE |
| 6 | `DMATRFMOFFS` | `0x114` | destination **IMEM** byte address (`0`) | local side of the transfer | SOURCE |
| 7 | `DMATRFFBOFFS` | `0x11c` | source offset in the image (`imem_base_img` = `0x100` for this booter) | external side | SOURCE + MEASURED† |
| 8 | `DMATRFCMD` | `0x118` | `IMEM=1`, `SEC=1`, `WRITE=0`, `SIZE=6` (=256 B), `CTXDMA=0` | one 256-byte block per write; 0x7600 bytes ⇒ 118 writes, offset advancing by 256 each time | SOURCE |
| 9 | `DMATRFCMD` | `0x118` | poll `FULL` (bit 0) = 0 before each write, `IDLE` (bit 1) = 1 at the end | writing while full corrupts the transfer | SOURCE |
| 10 | repeat 4–9 for **DMEM** | | `IMEM=0`, `SEC=0`, dest `0`, src offset `0x7700`, length `0x6000` | the DMEM image carries the signature patch site at +0x10 | SOURCE + MEASURED† |
| 11 | `BROM_PARAADDR(0)` | `addr2+0x210` → `0x841210` | `0x10` = offset of the signature inside the DMEM image | tells the boot ROM where to find the signature | SOURCE + MEASURED† |
| 12 | `BROM_ENGIDMASK` | `addr2+0x19c` | `1` (from the booter's meta dword 1) | engine id the signature must cover | SOURCE + MEASURED† |
| 13 | `BROM_CURR_UCODE_ID` | `addr2+0x198` | `3` (meta dword 2) | selects which fused key/version applies | SOURCE + MEASURED† |
| 14 | `MOD_SEL` | `addr2+0x180` | `1` = `RSA3K` | signature algorithm | SOURCE |
| 15 | `BOOTVEC` | `0x104` | `0x100` (= `app[0].offset`) | entry point, IMEM-relative | SOURCE + MEASURED† |
| 16 | `MAILBOX0` / `MAILBOX1` | `0x040` / `0x044` | low / high 32 bits of the **meta's system physical** address; `0`/`0` for a plain unload | the booter's only argument | SOURCE |
| 17 | `CPUCTL` | `0x100` | write bit 1 (`STARTCPU`) | start | SOURCE |
| 18 | `CPUCTL` | `0x100` | poll bit 4 (`HALTED`) = 1, bounded (see §5.3) | the booter exits/halts when done | SOURCE |
| 19 | `MAILBOX0` | `0x040` | read; **0 = success**, non-zero = booter error code | the ucode's own verdict | SOURCE |

Notes that matter for implementation:

- The **signature must be patched into the DMEM image before load**: the 384-byte
  production signature selected by fuse version is written at DMEM-image offset `0x10`
  (`= patch site 0x7710 − DMEM image base 0x7700`). The container carries 2 signatures of
  384 bytes each at file offset 60; pick with the same fuse-version rule the boot ROM
  will apply (§4.4). If you skip the patch, the boot ROM rejects the image.
- `SEC=1` on the IMEM transfer is what makes those 256-byte pages *secret* pages. A
  booter loaded with `SEC=0` will not authenticate.
- Falcon code loads are always 256-byte blocks, so both sizes must be multiples of 256:
  `0x7600` and `0x6000` are.
- **Pre-filling `MAILBOX0` with a sentinel is a cheap "did it even start" test** —
  NVIDIA's driver does exactly that for the ACR-style binaries, because 0 means success
  and an unstarted binary would otherwise look successful. Do the same: write a
  distinctive non-zero value, then require it to have been overwritten.

### 4.3 The radix3 tree (RM image delivery)

The ELF is 38 MB and must be reachable by the booter's DMA as a *contiguous* virtual
run. The meta gives a **system physical address of the tree root**, not of the image.

| level | content | tag |
|---|---|---|
| lvl0 | one page: 512 entries of 64-bit addresses pointing at lvl1 pages | SOURCE |
| lvl1 | same, pointing at lvl2 pages | SOURCE |
| lvl2 | 64-bit PTEs, each the **system physical** address of one 4 KiB page of the image, ascending | SOURCE |
| sizing | page count = ceil(size / 4096); entries per page = 512; the driver also rounds up for the tree's own pages | SOURCE |

Since the image is one physically-contiguous run in our loader (`nv_fw_load_blob`
already returns contiguous frames), each PTE is `base + i*4096`; the tree is pure
arithmetic, no mapping. **Tag: SOURCE for the shape, INFERRED that a linear run is
acceptable** — the reference driver builds it exactly this way and notes that GSP-RM does
not support discontiguous buffers today.

### 4.4 What the boot ROM actually validates

| check | mechanism | failure observable | tag |
|---|---|---|---|
| the image is authentic for **this** engine and **this** fuse version | boot ROM reads `BROM_PARAADDR(0)` → the 384-byte signature in DMEM, compares against keys fused into the GPU, using `ENGIDMASK` and `CURR_UCODE_ID` to select the key/version; `MOD_SEL` picks RSA-3K | core halts, `MAILBOX0` non-zero, no execution | SOURCE |
| the fuse version is one this signature claims | a fuse-version register at `0x8241c0 + (ucode_id−1)*4` yields a *bitmask* of supported versions; the signature's own fuse mask is ANDed with it and the first matching bit selects the signature slot | signature-selection failure ⇒ same halt path | SOURCE |
| the *right* signature section was patched in | the host's choice of `.fwsignature_<engine>` must match the chip: AD102 uses `.fwsignature_ad10x`, GA10x uses `.fwsignature_ga10x` — this image carries both | wrong section ⇒ authentication fails even though nothing else is wrong | SOURCE |
| the bootloader/RM images are authentic | verified by the **booter**, not by the boot ROM (it is the booter that consumes `sysmemAddrOfSignature` and the radix3) | booter reports via `MAILBOX0`; `verified` in the WPR meta stays 0 | SOURCE + INFERRED |

**Not established:** the exact error code values in `MAILBOX0`. Treat any non-zero value
as "authentication or layout rejection" and log it verbatim — the reference driver logs
and stops, it does not decode.

---

## 5. The bootargs the bootloader and GSP-RM consume

Two different argument blocks are involved, in two different address spaces. Neither is
the WPR meta.

### 5.1 GSP `MAILBOX0/1` → the libos init-argument array

Written **before** the booter is started (so that the bootloader/RM finds it already in
place). It points at an array of **four** region descriptors, 32 bytes each, in **system
memory**:

| off | size | field | value | tag |
|---|---|---|---|---|
| `0x00` | 8 | `id8` | the 8-byte tag spelled into a u64: `LOGINIT`, `LOGINTR`, `LOGRM`, `RMARGS` | SOURCE |
| `0x08` | 8 | `pa` | region **system physical** address, page-aligned | SOURCE |
| `0x10` | 8 | `size` | region size; the three log buffers are multiples of 4 KiB (64 KiB each is what the reference driver uses) | SOURCE |
| `0x18` | 1 | `kind` | `1` = contiguous | SOURCE |
| `0x19` | 1 | `loc` | `1` = sysmem | SOURCE |

Per-slot values that work (SOURCE, sizes chosen by the reference driver):

| slot | tag | pa | size | kind | loc |
|---|---|---|---|---|---|
| 0 | `LOGINIT` | own buffer | `0x10000` | 1 | 1 |
| 1 | `LOGINTR` | own buffer | `0x10000` | 1 | 1 |
| 2 | `LOGRM` | own buffer | `0x10000` | 1 | 1 |
| 3 | `RMARGS` | the `GSP_ARGUMENTS_CACHED` block, `0x1000` bytes | `0x1000` | 1 | 1 |

For the three log buffers the array's `pa` is followed by a **PTE array inside the
buffer**: the first 8 bytes of each buffer hold a write pointer, then one PTE per 4 KiB
page of that buffer. Cheap to get wrong, and it makes the GSP's first log line vanish.

### 5.2 `RMARGS` — the queue/buffer descriptor (535 layout)

**Version warning:** the layout below is the **535** one (what our firmware expects). The
*current* OpenRM headers show a newer revision that starts with magic/size fields and has
a different message-queue block. Do not mix them: with 535 firmware, a value written at
the wrong offset is read as `sharedMemPhysAddr` and the GSP faults on its first queue
access.

| off | size | field | value / meaning | space | tag |
|---|---|---|---|---|---|
| `0x00` | 8 | `sharedMemPhysAddr` | base of the one shared block that holds PTEs + cmd queue + status queue | **system physical** | SOURCE |
| `0x08` | 4 | `pageTableEntryCount` | number of 64-bit PTEs stored at `sharedMemPhysAddr` | — | SOURCE |
| `0x10` | 8 | `cmdQueueOffset` | offset of the command queue inside the block (`0x1000` with the reference driver's sizing) | byte offset | SOURCE |
| `0x18` | 8 | `statQueueOffset` | offset of the status queue inside the block (`0x41000`) | byte offset | SOURCE |
| `0x20` | 8 | `locklessCmdQueueOffset` | 0 (feature unused) | — | SOURCE |
| `0x28` | 8 | `locklessStatQueueOffset` | 0 | — | SOURCE |
| `0x30` | 12 | `srInitArguments` (`oldLevel`, `flags`, `bInPMTransition`) | zeros on a cold boot | — | SOURCE |
| `0x3C` | 4 | `gpuInstance` | 0 | — | SOURCE |
| `0x40` | 16 | profiler args (`pa`, `size`) | zeros unless profiling | — | SOURCE |

(Offsets in that table are for the 535 layout as the reference driver writes it; the
struct is 0x1000 bytes and the rest is zero. **Tag: SOURCE for fields and order,
INFERRED for the byte offsets** — derived from the field sequence and natural alignment
of the 64-bit members, which is what the GSP-RM build for 535 was compiled against.
Falsifier: the first RPC message arriving garbled, or the GSP faulting with a pointer
equal to the value we wrote at `0x00` shifted by 8.)

**The shared block itself** (all inside one physically-contiguous sysmem allocation):

| part | offset | size | content | tag |
|---|---|---|---|---|
| PTE array | `0x0000` | `0x1000` | 64-bit PTEs, one per 4 KiB page of the *whole block*, ascending | SOURCE |
| cmd queue | `0x1000` | `0x40000` | a queue header: `version=0`, `size`, `msgSize=0x1000`, `msgCount=63`, `writePtr=0`, `flags=1` ("swap RX"), `rxHdrOff` = offset of the peer read pointer, `entryOff=0x1000` | SOURCE |
| status (msg) queue | `0x41000` | `0x40000` | same header shape; its `readPtr` is what the host advances to consume events | SOURCE |

`flags=1` and `rxHdrOff` are the two fields that make the queues work in the reference
driver; a queue with `flags=0` will not report the peer's read pointer and the handshake
stalls.

### 5.3 The firmware version string — where it actually is, and where it is not

| item | value | tag |
|---|---|---|
| the version string `"535.113.01"` | lives in the RM ELF's `.fwversion` section, 11 bytes including the NUL | MEASURED† |
| is it passed to the bootloader/GSP by the host? | **no** — no field of the meta or RMARGS carries it | SOURCE |
| `FALCON_OS` (`0x080` of the GSP falcon) | gets the bootloader descriptor's `appVersion`, which is **0** for this firmware, so writing it is neither a version announcement nor a milestone | MEASURED† + SOURCE |
| where the host-visible version comes from | the RPC reply to the GSP init — that is what populates `nvidia-smi`'s "GSP Firmware Version" | INFERRED |

### 5.4 Which address space each address is in — summary

| consumer | field | space |
|---|---|---|
| SEC2 booter | `MAILBOX0/1` | system physical (meta) |
| SEC2 booter | all `sysmem*` meta fields | system physical |
| SEC2 booter | all VRAM-side meta fields | VRAM offset from FB base |
| GSP / GSP-RM | `MAILBOX0/1` (GSP) | system physical (libos array) |
| GSP-RM | RMARGS `sharedMemPhysAddr`, every `pa` in the libos array, every PTE | system physical |
| GSP-RM | RMARGS `*QueueOffset` | byte offsets inside the shared block |

---

## 6. The start step, and what proves it started

### 6.1 Order (host actions, no WPR2 writes anywhere)

| # | action | read-back that proves it | tag |
|---|---|---|---|
| 1 | reserve: RM image, bootloader, meta, libos array, log buffers, shared block — all physically contiguous | log the physical ranges and their sizes | — |
| 2 | build the radix3 tree for the RM image | walk your own tree and compare every PTE to the image's page addresses | — |
| 3 | fill the 256-byte meta (§3.3) | read it back from system memory and compare to a second encode | — |
| 4 | build libos array + RMARGS (§5) | same | — |
| 5 | reset the GSP, then run **FWSEC-FRTS** with `frtsOffset` — only if `frtsSize > 0` (it is 1 MiB on this chip) | FWSEC's own result (a secure-scratch/mailbox word) | SOURCE, *not implemented in HobbyOS* |
| 6 | reset the GSP into **RISC-V mode**: after the reset completes, write `NV_PRISCV_RISCV_BCR_CTRL` (`0x111668`) = `0x111` (bit 0 VALID, bit 4 CORE_SELECT=RISCV, bit 8 BRFETCH) | read `0x111668` back **immediately** and require `0x111`. (After GSP-RM is up the card is measured at `0x1`, so a late read proves nothing) | SOURCE |
| 7 | write GSP `MAILBOX0/1` = libos array physical address | read back both | SOURCE |
| 8 | run the SEC2 sequence of §4.2 (steps 1–19) | `MAILBOX0 == 0` | SOURCE |
| 9 | (optional) read the meta copy at `gspFwWprStart` through BAR1 | `verified == 0xA0A0A0A0A0A0A0A0` | INFERRED |
| 10 | write GSP `FALCON_OS` (`0x080`) = `appVersion` (= 0) | read back | SOURCE |
| 11 | wait for the **GSP init RPC event** on the status queue | an RPC element with `header_version = 0x03000000`, signature bytes `V R P C`, and the event function number for "GSP init done" | SOURCE |

### 6.2 What "it booted" looks like, in order of appearance

| milestone | what you read | strength | tag |
|---|---|---|---|
| SEC2 finished the booter | `MAILBOX0 == 0` after `HALTED` | **the** authoritative, cheap proof that WPR2 was built and the RM was accepted | SOURCE |
| meta accepted and locked | `verified` field = the A0A0… pattern in the WPR2 copy | strong, and readable without any protocol | INFERRED |
| GSP core is executing | `NV_PRISCV_RISCV_CPUCTL` (`0x111388`) bit 7 (`ACTIVE_STAT`). **But** this card reads `0x10` with GSP-RM demonstrably running, and most of that window reads `0xBADF5720` post-init — i.e. the word is inside a gated window. **Do not gate on it.** | weak on this card | SOURCE vs MEASURED, conflict in §7 |
| GSP-RM is alive and serving | first RPC event on the status queue (`GSP init done`) | definitive | SOURCE |
| GSP-RM is happy | the init RPC's reply completes and the next RPC (system-info) is answered | definitive | SOURCE |

### 6.3 Bounded waits and what to log

| wait | bound | on timeout, log | rationale |
|---|---|---|---|
| SEC2 `DMATRFCMD.FULL` clear, per 256-byte block | 10 ms per block | block index, `0x118`, `0x120` | a stuck queue means a programming error, not a slow engine |
| SEC2 DMA `IDLE` after the last block | 100 ms | same | — |
| SEC2 `CPUCTL.HALTED` after start | **2 s** (the reference driver's own bound) | `0x100`, `0x104`, `0x040`, `0x044`, `0x120`, `0x128`, and the whole 19-step trace of what we wrote | the booter either finishes in well under a second or has failed; 2 s is generous |
| `MAILBOX0` non-zero at halt | immediate | the raw value, plus "this is a booter error code, not a HobbyOS bug" | do not retry blindly |
| GSP init RPC | 2–5 s | status-queue `writePtr`/`readPtr`, the shared block's physical address, and the first 64 bytes at each queue head | distinguishes "queue never written" from "queue written, we mis-parsed" |

Also log, once, at the top of a boot attempt: the full 12-word PFB window (§2.4), the
computed layout (§3.2), and the meta digest. A wedged card's next session then starts
from data instead of archaeology.

---

## 7. Where sources and measurement disagree

| # | disagreement | resolution | tag |
|---|---|---|---|
| 1 | `0x1FA824/28` named WPR2 and used by RM as a **liveness predicate** (`HI.VAL != 0`) and as a **layout check** after FRTS (`LO.VAL == frtsOffset>>12`) vs. reading `0x1FFFFE00`/`0` on a card whose GSP-RM is demonstrably running | **prefer the measurement**: named WPR2 and (start,end) is established, but the words are not usable as an oracle from unprivileged BAR0 — RM's own predicate would call this working card "not up". Cause identified as read protection (`0x1FA7C8` bit 0 = 1). | SOURCE vs MEASURED |
| 2 | `0x111388` bit 7 = RISC-V ACTIVE (both drivers) vs. reading `0x10` with GSP-RM running | **prefer the measurement**: do not gate on bit 7. The window is gated (neighbours read `0xBADF5720`), so the word itself is suspect, and a working card reads it as "halted". | SOURCE vs MEASURED |
| 3 | `0x1668 = 0x111` "at BAR0+0x1668" vs. reading `0xBADF5040` there in both states | **both are right**: the write is GSP-block-relative (`0x111668` = `BCR_CTRL`), the old doc's absolute address was wrong. `0x111668` reads `0x1` post-init. | SOURCE + MEASURED |
| 4 | WPR layout: reserve PRAMIN size (16 MiB) at the top (NVIDIA) vs. 1 MiB (nouveau) | **prefer NVIDIA's rule** — it is the vendor of the ucode that validates the layout. Keep nouveau's variant in the log as a second candidate. | SOURCE vs SOURCE |
| 5 | `RMARGS` layout: 535 (no magic/size header) vs. current OpenRM headers (magic/size first) | **use the 535 layout**, because our firmware is 535.113.01. The newer header is a different interface revision. | SOURCE vs SOURCE |
| 6 | `docs/nv-gsp-firmware.md` §6's WPR meta hypothesis (`fb_size/wpr_start/wpr_size/region[3]`) | **superseded and wrong in shape**; real layout is §3.3. Magic, revision and 256-byte size were right. | SOURCE |
| 7 | `0x001668` "not a readable BAR0 word" | correct as stated, and it is not supposed to be: it is falcon-relative. Do not drop the finding — retarget the offset. | MEASURED |
| 8 | `docs/nv-gsp-firmware.md` §6's "WPR meta … 128 KiB top-down" | **superseded**: 128 KiB is the alignment of the region's **end**, and 1 MiB is the alignment of its **start** (the meta block is rounded up to 1 MiB). A "top-down by 128 KiB" rule would misplace the start by up to 896 KiB and shift everything the booter reads. | SOURCE |

---

## 8. Implementation order

The smallest set of steps from "firmware on disk" to "GSP says hello", each landing on
its own and each verifiable by a read-back that cannot pass by accident. Do not skip a
step because the next one "obviously works".

| # | step | cost | read-back / pass condition | failure means |
|---|---|---|---|---|
| 1 | **Layout arithmetic, pure function.** Given `fbSize`, `.fwimage` size, bootloader size, compute the nine VRAM offsets of §3.2 and assert the invariants: `wprEnd ≤ fbSize − 16 MiB`, `wprEnd` 128 KiB-aligned, `frts < bootBin < gspFw < heap < wprStart < nonWpr`, every region inside the next, no overlaps. | trivial | unit test with the pinned sizes; the numbers must equal the §3.2 column | arithmetic bug — the commonest way to wedge a boot |
| 2 | **Meta encode/decode round-trip** with the real field table (§3.3), replacing the `nv_fw.h` hypothesis. | small | encode, decode, compare field by field; assert the encoded buffer is 256 bytes and magic/rev/digest match a fixture | layout drift |
| 3 | **Radix3 builder.** PTEs for the 38 MB image, three levels. | small | walk the tree in a unit test and reconstruct every page address; compare with the image's real pages | a booter DMA fault much later, with no clue |
| 4 | **Read the 12-word PFB window and the SEC2/GSP identity words; log the state verdict** (§2.5). Read-only, safe on any card, runs every boot. | tiny | the log matches one of the four states, or says "unknown" | never boots, but also never breaks anything |
| 5 | **GSP into RISC-V mode** (`0x111668 = 0x111` after a reset), alone. | small | `0x111668` reads `0x111`; `0x100` bit 4 set | the switch did not take — and nothing after this can work |
| 6 | **SEC2 booter load, DMA only, no start.** Load IMEM+DMEM, program BROM registers, stop before `STARTCPU`. | medium | read back `DMATRFBASE/BASE1/MOFFS/FBOFFS`, the BROM words, `BOOTVEC`, `CPUCTL` (not started); no halt bit yet | a DMA or BROM programming error, caught before anything can run |
| 7 | **Start SEC2 and collect the verdict.** | medium | `HALTED` within 2 s, then `MAILBOX0 == 0` | non-zero ⇒ print it, stop, do not retry |
| 8 | **Read the WPR2 meta copy through BAR1** and check `verified`. Requires the BAR1 window rebased to cover the WPR region (§3.5) — until that exists, skip this step. | small | `verified == 0xA0A0…`, magic intact | booter rejected the meta, or our copy address is wrong |
| 9 | **Shared queues + RMARGS + libos array**, validated without booting: self-consistency (offsets inside the block, PTE count matches, queue headers agree). | medium | a static checker; no hardware needed | garbled first RPC |
| 10 | **GSP mailboxes + `FALCON_OS`, then wait for the init event.** | large | an RPC element with `0x03000000` / `VRPC` at the status-queue head | queue plumbing, not the booter |

Steps 1–4 need no hardware and no new register knowledge: land them first, because they
make every later failure interpretable.

---

## 9. What remains unknown, and why each one matters

| # | unknown | why it matters | how to settle it |
|---|---|---|---|
| 1 | whether the computed WPR2 base must match what the booter would compute, bit for bit, or whether it just has to be self-consistent | a mismatch is a failed boot whose only symptom is a non-zero `MAILBOX0` | compare our computed layout against a during-boot capture of `0x1FA824` **after** the privilege gate question is settled |
| 2 | the exact validation predicate the booter applies to the meta | determines how much of §3.2 must be exactly right | boot once with a deliberately-wrong-but-plausible `fbSize` and read the error code — **only on a card we can reset** |
| 3 | the meaning of `0x1FA804/08/0C/14/18/40` | if one of them is an *enable* or a *fence* for WPR2, a boot decision may hang off it | the read-only dump in §2.4(c), including a during-boot pass |
| 4 | whether `LO`/`HI` write order matters, and whether writes are honoured at all while `0x1FA7C8` bit 0 is set | only matters if we ever program WPR2 ourselves | do not test on r1 |
| 5 | `MAILBOX0` error-code values | turns "boot failed" into "signature rejected" vs "bad layout" | read the booter's error list from a source that documents it; until then log raw |
| 6 | whether FWSEC-FRTS is mandatory on AD102 with 535 | the reference driver runs it when `frtsSize > 0` (1 MiB on this chip), so skipping it may leave the GSP without FRTS data | a boot with `frtsSize = 0` and the same firmware; compare the first RPC reply |
| 7 | the real `RMARGS` byte offsets for 535 | one wrong offset and the GSP reads a pointer as flags | disassemble-free check: the first RPC reply's plausibility; or a source dump of a 535 driver's RMARGS buffer |
| 8 | whether `0x111388` bit 7 is EVER set during a live boot | it is the only cheap "core started" check, and it currently contradicts the measurement | tight-loop sampling of `0x111388` around a known-good driver load |
| 9 | whether BAR1 maps VRAM 1:1 above 4 GiB on this card | every VRAM write depends on it | pattern write/read-back once BAR1 windows are in place |
| 10 | the booter's `flags`/`pagingConfig` semantics beyond "write 0" | only if recovery/ECC paths are ever used | not needed for the happy path |
| 11 | **which register re-bases the BAR1 window on AD102**, and whether writes through a rebased window reach VRAM 1:1 | without it we cannot write or read anything at 24 GiB — the meta read-back and every "prove the write took" step is blocked | its own lane: find the PFB BAR1 base/window registers in the published headers, pattern-test one page |

---

## 10. Sources read (named, so each claim above is checkable)

Nouveau, in the Linux tree (`drivers/gpu/drm/nouveau/`):

- `nvkm/subdev/gsp/rm/r535/gsp.c` — `r535_gsp_init` (the `FALCON_OS` write and the init
  RPC poll), `r535_gsp_shared_init`, `r535_gsp_libos_init`, `r535_gsp_rmargs_init`,
  `r535_gsp_set_rmargs`, and the shared-queue setup.
- `nvkm/subdev/gsp/tu102.c` — `tu102_gsp_oneinit` (the FB layout arithmetic),
  `tu102_gsp_wpr_meta_init` (field-by-field meta fill), `tu102_gsp_init` (booter launch
  with the meta address in SEC2's mailboxes), `tu102_gsp_booter_unload`,
  `ga100_gsp_mmu_lock_lo`.
- `nvkm/subdev/gsp/ad102.c`, `ga102.c` — which HAL AD102 selects
  (`ga102_gsp_booter_ctor`, `.fwsignature_ad10x`, `ga102_gsp_reset`'s `0x1668` write).
- `nvkm/subdev/gsp/base.c` — the GSP falcon's BAR0 base (`0x110000`).
- `nvkm/subdev/gsp/fwsec.c` — the FRTS/SB commands and what they take.
- `nvkm/falcon/ga102.c` — the DMA registers, the `XBAR`-style transfer command encoding,
  the BROM parameter writes, and the boot sequence (`BOOTVEC`, `CPUCTL`, mailbox).
- `nvkm/falcon/gm200.c` — `gm200_flcn_fw_boot` (mailboxes, entry, start, halt wait,
  mailbox check).
- `nvkm/falcon/fw.c` — signature patching before load, and the HS-v2 load-header decode.
- `include/nvfw/hs.h`, `include/nvfw/fw.h` — the descriptor layouts.
- `nvkm/subdev/gsp/rm/r535/nvrm/gsp.h` — `GspFwWprMeta`, the libos array, 535's RMARGS,
  the queue headers, the heap-sizing constants.

NVIDIA `open-gpu-kernel-modules` (MIT), `src/…`:

- `kernel/gpu/gsp/kernel_gsp.c` — the boot flow comments, "unexpected WPR2 already up",
  FWSEC-FRTS before the booter, booter/unload roles.
- `kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c` — `kgspPopulateWprMeta_TU102` (the
  authoritative layout, the layout diagram, the 256 MiB scrub rule, the MMU_LOCK clamp),
  `kgspBootstrap_TU102` (order of RISC-V switch, mailboxes, booter, `FALCON_OS`,
  RISC-V-active check, init RPC), `kgspTeardown_TU102`.
- `kernel/gpu/gsp/arch/turing/kernel_gsp_booter_tu102.c` — booter load/unload on SEC2,
  mailbox arguments, "non-zero mailbox0 = booter error".
- `kernel/gpu/gsp/arch/ampere/kernel_gsp_falcon_ga102.c` — the HS-falcon execution
  sequence, BROM parameter registers, DMA loops, the "pre-fill mailbox with
  not-started" trick.
- `kernel/gpu/falcon/arch/ampere/kernel_falcon_ga102.c` — `kflcnResetIntoRiscv_GA102`
  and `kflcnRiscvProgramBcr_GA102`.
- `kernel/gpu/vbios/arch/turing/kern_vbios_tu102.c` — the FWSEC-FRTS post-check: reads
  the WPR2 pair and requires the start to equal `frtsOffset >> 12` and the end to be
  non-zero; also the SB path's secure-scratch checks.
- `kernel/gpu/bus/arch/hopper/kern_bus_gh100.c` — `kbusCarveoutWprs_GH100`: the only
  place that spells out how the pair is decoded (start from LO, end from HI plus the
  128 KiB block granularity), which is what establishes (start, end) rather than
  (low, high).
- `inc/kernel/gpu/gsp/gsp_init_args.h` — the current `GSP_ARGUMENTS_CACHED` and
  `MESSAGE_QUEUE_INIT_ARGUMENTS` (the newer revision, see §5.2).
- `arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta_v1.h` — the V1 meta used by "Booter
  (Turing through Ada)"; `gsp_fw_wpr_meta.h` — the newer, larger revision.
- `src/common/inc/swref/published/turing/tu102/dev_fb.h`,
  `.../ampere/ga100/dev_fb.h`, `.../hopper/gh100/dev_fb.h` — WPR1/WPR2/LOCK register
  names, field placement, reset value, privilege mask.
- `.../ampere/ga102/dev_falcon_v4.h`, `.../ga102/dev_falcon_second_pri.h`,
  `.../ga102/dev_riscv_pri.h`, `.../ga102/dev_gsp_addendum.h`,
  `.../ada/ad102/dev_gc6_island*.h`, `.../ga102/dev_fbif_v4.h` — the exact register
  offsets and bit fields used in §4.2 and §6.

envytools (public register/tooling docs, not driver code):

- `rnndb/falcon.xml` — `UC_CTRL` bit meanings, `XFER_*` bit fields, `MEMIF`.
- `docs/hw/falcon/xfer.rst` — the code/data transfer contract: 256-byte code blocks,
  `4<<size` data blocks, `ext_base << 8`, the secret flag.
- `docs/hw/falcon/proc.rst` — RUNNING/STOPPED/SLEEPING transitions.

Local:

- `docs/nv-gsp-reference.md` (the MEASURED values, §3.6 and §5), `docs/nvidia-driver.md`
  §5.5 (the cross-check between two stacks on one card), `src/include/nv_fw.h` and
  `src/kernel/nv_fw.c` (the parser and the WPR-meta hypothesis this doc replaces),
  `src/kernel/nv_gsp.c` (the staged boot machine), `src/kernel/nv_regs.h`.
- The pinned blobs: `third_party/nvfw/ad102/gsp/{booter_load,booter_unload,bootloader}-535.113.01.bin`
  and `third_party/nvfw/ga102/gsp/gsp-535.113.01.bin`.
