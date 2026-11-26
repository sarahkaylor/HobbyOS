# The ladder, re-pointed: nine stranded native rungs and the measured rows that replace them

> **Coordinator note (2026-11-26).** This is the re-point record cited by
> `docs/nv-plan.md` §2.13. It was written by card `t_96d8983f`, whose §4 block is
> a *proposal*; that text has since been landed, edited, as plan §2.13, and the
> nine cards it names are now blocked `kind=capability`. Read the plan for the
> authoritative wording. Copied into the repo unmodified apart from this note.

Card `t_96d8983f` — the pre-committed fallback for the **last lever** on the GB202
ACR wall. Its parent, `t_320e84e5` (the 620-generation pair: 620 FMC + 620 boot
image + recovered `RM_RISCV_UCODE_DESC` v7 offsets), closed **negative**, so this
card runs. If it had closed positive this card would have been archived unanswered
(naming `t_23512cd0` as the successor on that branch); it did not, so the pivot is
executed here.

**What this document is.** One row per open card that names AD102 or GB202 hardware
in its acceptance text, saying which rung it encodes, which measured wall it sits
behind, and **which already-measured fallback row replaces it**. A rung whose
acceptance names a part cannot be re-pointed by reassignment — the part is inside
the criterion — so the honest move is to record it as parked and point the claim at
a number that exists. Nothing here changes a card's status, assignment or workspace.

**What this document is not.** It is not a cancellation of the native branch, not a
claim that the fallback is NVIDIA's driver, and not a substitute for the park
record. The park record is `docs/gb202-rm-parked.md` (`t_251d3c71`); the ladder
itself is `docs/nv-games-ladder.md` (its §2.2 is the fallback table quoted below).

**Nothing was booted for this card.** No VM 300, no VM 313, no qemu. Every number
in this file was measured by an earlier card and is cited to the document or log
that carries it; no new measurement was made and none is implied.

Part discipline, as in the park record: AD102 numbers are AD102 (`10de:2684`,
r1 `01:00.0`, VM 300) and GB202 numbers are GB202 (`41:00.0`, VM 313). The two
fallback rows are **neither** — they ran on virtio GPUs, and they say so.

---

## 1. The two walls this ladder is behind

Both are measured, both are invariant across every variation the lane could supply,
and neither has a host-side lever left.

| wall | part | measured invariant | where it is recorded |
|---|---|---|---|
| **AD102 booter wall** (`t_9bf9bc0c`, blocked) | AD102 `10de:2684`, VM 300 | SEC2/the boot ROM rejects **every reachable vendor booter** with `0x002B0009` at +4.2 ms with `EXIT 0x10`, over three AD102 builds, while a GA102 image returns `0x00250009` — the gate discriminates by part, so it is authorisation, not a malformed container | `t_67f09feb`; quoted in `docs/gb202-rm-parked.md` §6.1 |
| **GB202 ACR wall** (`t_a769beaa` blocked; `t_251d3c71` park) | GB202 `41:00.0`, VM 313 | 570-generation FMC: every attempt with every host-supplied value accepted ends `MAILBOX0 = 0x0B` in 0.2–55 s **with the PFB WPR2 pair torn down in the same row** (`wpr2_lo=0x7FFFFE00 wpr2_hi=0`), `statq_written=0 linked=0 initdone=0`. 620-generation FMC + 620 boot image (`t_320e84e5`, mode 62): `cot answer errorCode=0`, but `MAILBOX0=0x824`, `0x14DC=0x12000824` (variant 1, partition 1) at all 11 samples, WPR2 down — identical to the 570-boot-image control, i.e. `0x824` is emitted **before** the boot image matters | `docs/gb202-rm-parked.md` §1–§5; `t_320e84e5` (report `docs/gb202-fmc620.md`, logs `5090-gb202fsp-fh60b.log`, `…-fh62.log`) |

The one line that matters for the rows below: **there is no live GSP-RM peer on
either part**, so every rung whose acceptance asks for an object, a channel, a
submit, a frame or a fps number *from the `nv` driver* is behind one of those two
rows — and the 620 box did not move it, it named it earlier (`0x824`).

The 620 box also settled the last cheap hope: the vendor's own FSP is not stageable
by any host (`kern_fsp_gb202.c`: the BootFSM starts it out of chip reset; 620.70.10
ships no FSP image bindata), so the wall's remaining variable cannot be varied from
the host at all. `0x824`'s meaning is not obtainable from the host side either.

---

## 2. The nine open cards, one row each

Fallback rows are quoted from `docs/nv-games-ladder.md` §2.2 and re-derived in
`docs/gb202-rm-parked.md` §6.2:

* **N1v — RimWorld main menu, 720p: 51.02 fps median** (7,600 presents / 149,848
  guest ms, guest clock only), **`renderer=virgl`** (host **llvmpipe**; r1 has no
  host 3-D driver for the 4090), VM 311 `hobbyos-games`, `virtio-vga`, no
  passthrough.
* **N8v — Cyberpunk 2077's own benchmark, 1280×720, Custom/Low, scaling off: 29.19
  fps avg / 29.66 median / 18.18 1 %-low**, **`renderer=venus`** (virtio-gpu-gl-pci,
  `venus=true`) on the **AMD** host (`RX 9070 XT`, RADV). `summary.json`
  `averageFps = 29.189037322998048`; `frames.csv` median frame time 33.72 ms →
  29.656 fps, p99 55.0008 ms → 18.182 fps. `docs/nv-plan.md` §2.5 and the ladder
  round these to **29.7 / 18.2**; the unrounded digits are the ones this document
  uses because they are the ones the frames give.

| card | assignee | rung it encodes | wall it is behind | replaced by (measured, labelled) |
|---|---|---|---|---|
| `t_470a9443` — *RM objects: channel, VA space and an engine class from GSP-RM* | nv-card | **N4-style RM minting** (the rung that makes the mock RM stop being enough: object handles from the firmware, a VA bound to real VRAM) | AD102 booter wall `t_9bf9bc0c` (`0x002B0009`); no GSP-RM peer exists to mint from on either part | **N3v — `[VIRGL3D] match=4096/4096`** (virgl 3-D submit + read-after-clear, kernel only, no Mesa), `renderer=virgl`. No measured row allocates RM objects, and none is claimed: N3v is the measured substitute for *a driver-owned submit path existing at all* |
| `t_a350b5ac` — *First submit: a 64×64 clear reaches the engine and reads back* | nv-card | **L3 / N5n** (first GR submit, clear reads back — "not a fence word, not a zero") | AD102 booter wall `t_9bf9bc0c`; its channel comes from `t_470a9443`, i.e. from GSP-RM | **N3v — `[VIRGL3D] match=4096/4096`**, `renderer=virgl`. This is the only measured read-after-clear on the board; it is kernel-virgl, **not** `nv` |
| `t_9e711a78` — *nvk enumerates AD102 (NVK)* | nv-shim | **N6n / L4** (stock nvk gets past the engine-class object `0xc7b5 ampere_dma_copy_b` and lists `AD102 (NVK)`) | AD102 booter wall `t_9bf9bc0c`; the class object is refused by name `need=gsp_rm` | **N6v — `vkcube` draws and presents**, `renderer=venus` (two screendumps, frames > 0; P5/P6 harness in `tools/cp/venus/`). The device listed is Venus, and no `AD102 (NVK)` row exists |
| `t_652caa8d` — *First frame: vkcube draws and presents* | nv-plat | **N6n (native frame)** — the one observation where VRAM, VA, submit, fences, shader upload and present all work at once | AD102 booter wall `t_9bf9bc0c` (needs `t_9e711a78`) | **N6v — `vkcube` draws + presents, `renderer=venus`**. Same gate, different renderer; the native frame is not claimed |
| `t_b410e3f5` — *Present path at 1080p through the driver, not the virtio scanout* | nv-plat | **present ownership** (driver owns scanout; a pixel at 1920×1080 seen in a screendump with the scanout named) | AD102 booter wall `t_9bf9bc0c` (needs `t_652caa8d`); the `nv` driver owns no display object and has never presented | **N0 — 1080p present gate, `(1919,1079)=(255,0,0)` in a QMP screendump** (the only measured 1080p present) **+ N1v — 51.02 fps median, `renderer=virgl`** for fps *with present included at 720p*. **The 1080p-through-the-driver claim is withdrawn, not replaced**: the fallback has a 1080p present gate and a 720p fps row, and no 1080p fps row was ever measured |
| `t_c38b0d00` — *RimWorld on the nv driver at 720p, menu at or above 30 fps* | nv-plat | **N7n / N1 at 720p** (menu ≥30 fps median from the guest's own present counter, input working, screendump looked at) | AD102 booter wall `t_9bf9bc0c` | **N1v — 51.02 fps median, `renderer=virgl`** (host llvmpipe, no passthrough), audio not reachable and not claimed. Gate met on the fallback; **not** on `nv` |
| `t_2bee66b5` — *Cyberpunk 2077 on the nv driver, ending in its own benchmark* | nv-plat | **N8n** (vkd3d-proton on the `nv` Vulkan driver reaches the game's own benchmark, numbers labelled `nv`) | AD102 booter wall `t_9bf9bc0c` (needs `t_652caa8d`, `t_a350b5ac`) | **N8v — 29.19 avg / 29.66 median / 18.18 1 %-low, `renderer=venus`** (AMD `RX 9070 XT` host). This is the comparison row the card itself quotes; it is the platform's number, not the driver's |
| `t_430cb397` — *Reasonable performance: 720p at or above 30 fps including present* | nv-plat | **N9n / L5** — the owner's definition of done, measured on `nv` with the bottleneck named | AD102 booter wall `t_9bf9bc0c` (needs `t_c38b0d00`, `t_2bee66b5`) | **N9v — met in part**: RimWorld **51.02 fps median `renderer=virgl`** clears ≥30; Cyberpunk **29.19 avg / 29.66 median / 18.18 1 %-low `renderer=venus`** clears ≥30 on avg and median and misses it on the 1 %-low. The rung is **partially met on the fallback, not met natively** — that split is the honest row |
| `t_938ced62` — *GB202: nvk enumerates GB202* | nv-card | **N6n on GB202** (successor of `t_9e711a78`; `vulkaninfo` lists `GB202 (NVK)` with quoted `apiVersion`/`driverVersion`, no stub ICD) | **GB202 ACR wall**: `0x0B` on 19 boots (570 FMC), `0x824`/`0x14DC=0x12000824` on the 620 pair — `statq_written=0`, so there is no GSP-RM peer to enumerate from | **N6v — `vkcube` draws + presents, `renderer=venus`**. Its own body already says a `need=gsp_rm` refusal is not the acceptance; a boot now would only re-measure the wall |

**Reading the table as a whole.** Four of the nine (the four fps/game rungs) map
onto rows that already exist with a renderer label. Two (RM objects, first submit)
map onto N3v, which is a measured *kernel* submit and not an RM object allocation —
the honest reading is that the fallback has no equivalent rung and N3v is the
nearest measured thing, not a substitute for the claim. One (1080p present through
the driver) is **withdrawn** because no fallback row measures 1080p fps. Two (native
`vkcube`, 1080p present) map onto N6v/N0 at the same gate with a different renderer.

**Not in the table, deliberately:**

* `t_a769beaa` and `t_9bf9bc0c` — the two **walls**, already blocked, not reopened,
  not re-pointed: they are the reason the rows above are parked.
* `t_23512cd0` — the **positive-branch** sibling minted under `t_320e84e5` (GB202 RM
  objects on the 620 pair). Its own run condition says it does not boot when the
  parent closes negative; it closed on that clause (no boot spent, the parent's
  mode-62 log quoted instead) and names this document. It is not one of the nine.
* `t_251d3c71` — the park record card, closed. This document is downstream of it.

---

## 3. What each of the nine cards now carries

Each of the nine got one comment (2026-11-26) stating, in one line: the native rung
behind it is **parked with a measured reason**, the reason is in
`docs/gb202-rm-parked.md` and this file, and **a mock or shim object — or a number
from another renderer — is not acceptance**. No card's status, assignee, parent
edge, workspace or attachment was changed; `nv-plat`/`nv-shim` cannot be dispatched
on this host anyway (only `nv-card`/`nv-card2` tick), and reassigning them would
spawn part-specific work against the wrong part, which is the opposite of the point.

Two pointers were also posted: one on `t_a769beaa` (the GB202 wall card) and one on
the parent `t_320e84e5`, both to this document.

---

## 4. PROPOSAL for `docs/nv-plan.md` §2.13

> **This is a proposal only.** `docs/nv-plan.md` is **coordinator-owned** and was
> **not edited** by this card — not one byte. The copy in the card trees
> (`docs/nv-plan.md`, 30,726 B) ends at §2.6; the §2.7…§2.13 numbering is the
> coordinator's newer draft, cited by cards `t_78a50be7`, `t_251d3c71` and this
> one. Insert after the last landed §2.x round section, renumber, or discard — but
> do not read the block below as landed text.

> **§2.13 The ACR wall is closed as a decision, and the ladder is re-pointed onto
> the fallback.** The last lever on the GB202 ACR wall was spent by `t_320e84e5`
> (620-generation pair) and closed **negative**: the 620 boot image's
> `UCODE_DESC_PROD` decodes cleanly (29-byte raw-deflate of `RM_RISCV_UCODE_DESC`
> v7, offsets `manifest 0/0xA00`, `monitorData 0xA00/0xE800`, `monitorCode
> 0xF200/0x37000` — the same three the 570 build declares), the FSP authenticates
> the 620 FMC (`cot answer errorCode=0`, `FSP_OK`), and the attempt still ends at
> `MAILBOX0 = 0x824` with `NV_PBUS_ZB_SW_SCRATCH_GSP_FMC_ERROR (0x14DC) =
> 0x12000824` (variant 1, partition 1) and the PFB WPR2 pair down at all 11
> samples — **identical to the 570-boot-image control**, so `0x824` is emitted
> before the boot image matters. The vendor FSP itself is not stageable by any host
> (`kern_fsp_gb202.c`: BootFSM starts it out of chip reset; 620.70.10 ships no FSP
> image bindata), so the remaining variable cannot be varied. Together with the
> 570-FMC ladder (`0x0B`, 19 boots, WPR2 torn down; `0x824` never appears there)
> and the AD102 booter wall (`0x002B0009`, three builds), **no host-side lever on
> either part moves the native branch off L1 + the measured part of L2**. Do not
> re-spend boots on: the heap pair, `fbSize`, `frtsSize`, `gspRmDescTarget`, the
> FRTS object or any `frtsVidmemOffset`, signature/ELF/boot-image identity or
> address, the boot image's container form vs. the 620 generation, the WPR2 band
> census, a second COT as a sweep, or any 6xx-FMC-with-570-inputs variant. **The
> board is re-pointed, not re-scoped:** the nine open rungs that name AD102 or
> GB202 in their acceptance text — `t_470a9443`, `t_a350b5ac`, `t_9e711a78`,
> `t_652caa8d`, `t_b410e3f5`, `t_c38b0d00`, `t_2bee66b5`, `t_430cb397`,
> `t_938ced62` — are recorded as **parked with a measured reason**, each pointing
> at the measured fallback row that replaces it: **RimWorld 51.02 fps median
> `renderer=virgl`** (host llvmpipe, no passthrough, audio not reachable) and
> **Cyberpunk 29.19 avg / 29.66 median / 18.18 1 %-low `renderer=venus`** (AMD
> `RX 9070 XT` host; the plan rounds these to 29.7 / 18.2). One rung is **withdrawn
> rather than replaced**: present at 1080p *through the driver* has no fallback
> fps row — the fallback has the N0 1080p present gate and a 720p fps row, and
> that is all it has. N9 is therefore **partially met on the fallback** (RimWorld
> clears ≥30 median; Cyberpunk clears it on avg and median and misses on the 1 %
> low) and **not met natively on either part**. The rule that keeps this row
> honest: **a mock RM object, a stub ICD, a shim's own refusal satisfied by
> construction, or a fps number produced on another renderer is not acceptance**
> for any of the nine. Record of every row: `docs/gb202-rm-parked.md` (wall),
> `docs/nv-ladder-repoint.md` (this re-point), `docs/nv-games-ladder.md` §2.2
> (the fallback rows).

---

## 5. Where everything is

| what | path |
|---|---|
| this document | `docs/nv-ladder-repoint.md` — this card's workspace: `WS/t_96d8983f/docs/`; also attached to `t_96d8983f` |
| park record (the wall) | `docs/gb202-rm-parked.md` — `WS/t_251d3c71/`, attached to `t_251d3c71` |
| the fallback ladder | `docs/nv-games-ladder.md` — attached to `t_1044c642` |
| 620-pair report + logs | `ATT/t_320e84e5/gb202-fmc620.md`, `…/5090-gb202fsp-fh60b.log`, `…/5090-gb202fsp-fh62.log`, `…/FMC620.BIN`, `…/BOOT620.BIN` |
| GB202 RM-objects report + logs | `ATT/t_a769beaa/REPORT-gb202-rm-objects.md`, `…/KEY-ROWS-gb202-rm-objects.txt`, `…/5090-gb202fsp-rm{50,51,53,54,55,56,57,59,59b}.log` |
| 620-pair positive-branch card (closed unspent) | `WS/t_23512cd0/tree/docs/gb202-rm620.md` |
| heap + prologue + FMC reports | `ATT/t_7178dca2/gb202-fwheap.md`, `ATT/t_0936a17f/REPORT-gb202-acr-prologue.md`, `ATT/t_a1c5bdd8/REPORT-gb202-fmc.md` |
| games evidence | `docs/cp-bench.md`; `docs/cp-bench-evidence/hobbyos-venus-720p/{summary.json,frames.csv,qemu-cmdline.txt}`; `docs/nv-plan.md` §2.5 / N1 |

`ATT/<card>/<file>` = `/srv/hermes-kanban/boards/hobbyos-nv/attachments/<card>/<file>`;
`WS/<card>/…` = `/srv/hermes-kanban/boards/hobbyos-nv/workspaces/<card>/…`.

Nothing was deleted: no branch, workspace, image, attachment or log was touched. No
VM was started, no image was built, `docs/nv-plan.md` was not edited, and no card
was reopened, unblocked or reassigned. The nine cards keep their status, their
assignee and their parent edges; they gained one comment each.
