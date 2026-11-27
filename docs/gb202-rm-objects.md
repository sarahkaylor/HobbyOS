# GB202 (RTX 5090 `41:00.0`, VM 313) — every FMC / boot-image / ACR blob reachable, and the two abort points

Card `t_a769beaa`, reopened by the owner on 2026-11-27 (**R4**, `docs/nv-plan.md`
§2.13.1). The reopening's first step was a **zero-boot survey** (§1–§10); the
round after it spent **2 boots** on the one variable that survey named (§11:
`615.78.08`, then the same state with the FSP's CMS2 window added), and the
round after **that** spent **1 boot** on the last one-field difference from the
vendor's 615 stock (§12: `nonWprHeapSize`). Every hardware row is quoted from a
log and labelled with its boot; every static fact names its source.

**Part discipline.** Every hardware row is **GB202, `41:00.0`, VM 313**. No AD102
number is used as a GB202 number. Closing state of this run: VM 313 `stopped`,
VM 300 `stopped`, `pgrep -c qemu-system-x86` = **0** on r1 and on this worker.

---

## 0. The answer, in six lines

1. **Nothing newer than the 620.70.10 set exists.** `620.73.04` is the newest
   public tag by version (`open-gpu-kernel-modules`, commit `a4b95f6a2d`; its tag
   commit, 2026-11-18, is even older than `620.70.10`'s 2026-11-23, which does
   not matter — see the next sentence). Its GB202 FMC and boot-image bindata are
   **byte-identical** to `620.70.10`'s: file sha256 `f36554a8…` / `1053c2dc…`,
   decoded FMC image sha256 `55042efa…`, boot image `fc42d268…`. So does
   `620.70.06`. A boot spent hunting an FMC newer than 620 buys nothing.
2. **The whole reachable corpus is 11 distinct GB202 FMC images and 11 distinct
   boot images**, across every public tag from `570.86.16` to `620.73.04` (§2).
   Only **four FMC images have ever been on the card**: `570.144` (`e4d17650…`,
   199,240 B — the `0x0B` ladder), `580.178.04` (`cea2190c…`, 215,624 B — `0x5`
   once on the pre-FRTS COT, then **`0x0B`** on the `0x0B` COT, §13),
   `615.78.08` (`5b74fefd…`, 287,304 B — `0x824`, §11) and `620.70.10`
   (`55042efa…`, 287,304 B — `0x824`).
3. **The abort point follows the FMC image, not the artifact set.** With the same
   host state (COT shape, boot-args page, GSP-RM image/signature and every
   descriptor field except the bootloader geometry that the paired image
   carries), the 570 FMC ends `0x0B` and the 620 FMC ends `0x824` at 150 ms
   (§5); **the 615.78.08 FMC, with its own boot image, also ends `0x824` at
   150 ms** (§11) — so `0x824` belongs to the 287 KiB FMC class, not to 620. The
   boot image, the signature/ELF and the ACR cannot move either code; the only
   host-substitutable input that changes the code is the FMC itself. **The
   `580.178.04` FMC — the 215 KiB class, and the image NVIDIA's own
   `nvidia-open-580.178.04` binds for this part — ends `0x0B` with WPR2 armed at
   the exact pair the 570 FMC produces** (§13). The class line is therefore
   bracketed from both sides: 199/215 KiB → `0x0B` with WPR2, 287 KiB → `0x824`
   with WPR2 never armed; the one class member in between is 590's 286 KiB
   (§6 rank 1).
4. **There is no GB202 ACR blob to enumerate.** No `BooterLoadUcode`,
   `BooterUnloadUcode` or `Sec2Bl` for GB100/GB202 exists in **any** of the 15
   tags' generated trees, in linux-firmware, or in either `.run`; GB202 exposes
   no SEC2 window (`0x840000` = `0xBADF4100` 7/7, measured `t_67f09feb`), and its
   FSP is started by BootFSM out of chip reset and **cannot be staged**.
5. **The one boot that was left has been spent, and it answered:** the
   `615.78.08` FMC (`5b74fefd…`) with its own boot image (`37159b4b…`), on the
   COT that yields `0x0B`, ends **`0x824` / `0x14DC = 0x12000824` at 150 ms**
   (§11) — identical to the 620 pair. `0x824` is therefore not a 620 regression;
   it is the 287 KiB FMC class's entry-path abort. Both `WprMeta` fields in
   which our block differed from the vendor's 615 stock are now answered — one
   by the corpus (decorative), one by measurement (§12: inert) — so the abort
   sits upstream of descriptor content. A third boot then spent the survey's
   rank-2 pair — NVIDIA's own `580.178.04` FMC with the boot image every wall
   boot already used — and answered `0x0B` with WPR2 armed (§13), which is why
   the boundary is now 580→590 and no longer 615→620.
6. **The comparison that decides whether the FMC or our host structures is the
   wall needs NVIDIA's own path on this part**: VM 301 (Ubuntu 24.04) with
   **`nvidia-open-580.178.04`** — the closed module refuses a GB202 (measured,
   `docs/nv-r1-host-renderer.md` §1.1) — with `41:00` passed through. That is the
   only success path on this silicon, and its FMC is statically known to be
   `cea2190c…` (§7).

**Acceptance is not claimed and not moved by this document.** No channel, VA
space or engine class exists, because GSP-RM has still never started.

---

## 1. What was searched, and what is reachable on 2026-11-27

The survey treated "reachable" as *every path this lane can read without a GPU
boot*: the worker's disks, r1 as root, the public NVIDIA source tags, the driver
packages (`.run`), linux-firmware, and the guest images.

| source | what is there | what it yielded |
|---|---|---|
| this worker, `/srv/nv5090/work/dl/` | `NVIDIA-Linux-x86_64-570.144.run` (375,742,470 B), `NVIDIA-Linux-x86_64-580.178.04.run` (397,214,534 B), extracted trees `x/` (570), `y/` (580) | the two `.run`s ship **only** `firmware/gsp_ga10x.bin` (570: 63,571,696 B `a8c3ebee…`; 580: 75,028,464 B `3b89a63f…`) and `gsp_tu10x.bin`; no separate FMC, boot or ACR blob |
| this worker, workspace `fw/` | containers `FMC.BIN` 199,820 (`cb59a35c…`, linux-firmware 570.144), `FMCV.BIN` 216,204 (`6adbd92d…`, 580.178.04), `FMC595.BIN` 287,884, `FMC620.BIN` 287,884; decoded images `FMC580.BIN` 215,624 (`cea2190c…`); staged images `BOOTIMG.BIN` 217,088 (`ac068832…`), `BLB.BIN` 200,812 (`d40b48e4…`), `BOOT620.BIN` 290,816 (`fc42d268…`); bindata for 580 and 595 | the exact artifacts the wall's boots staged (§2.2); the 580/595 bindata |
| this worker, `cache/scratch/t620/` | 620.70.10 GB202 bindata (Boot, Fmc, CcFmc, ConcatenatedFMC + descs) | 620.70.10 row of the table |
| r1, as root | `/lib/firmware/nvidia/gb202/gsp/` = `bootloader-570.144.bin` 200,812, `fmc-570.144.bin` 199,820, `gsp-570.144.bin` → `../../ga102/gsp/gsp-570.144.bin`; `pve-firmware` 3.18-7; no `.run`, no `nvidia-open` source tree, no DKMS module | linux-firmware's GB202 directory is the **570.144 set only** — the same bytes already on the card; nothing newer |
| r1, VMs | 313 disk `/root/hobbyos5090/disk.img` (the staged `FMC/BLB/BOOTIMG` set), 301 disk `local-lvm:vm-301-disk-0` (Ubuntu 24.04 + 580.178.04) | 313's staged set is §2.2; 301's 580 firmware is the same 75 MB `gsp_ga10x.bin` already extracted (no need to mount) |
| GitHub `NVIDIA/open-gpu-kernel-modules` | tags back past 550 (two API pages); every tag ≥ `570.86.16` carries `g_bindata_kgspGetBinArchiveGspRm{Boot,FmcGfwProdSigned}_GB202.c`; newest `620.73.04` | the 15-tag matrix in §2.1–§2.2 (files fetched by `raw.githubusercontent`, no API budget spent on blobs) |
| linux-firmware | `nvidia/gb202/gsp/{bootloader,fmc,gsp}-570.144.bin` (r1, above) | 570.144 only |
| GB202 VBIOS / ACR ucode | **no dump exists in the repo** (`docs/nv-vbios-evidence/` holds the 4090's AD102 VBIOS only); the 5090's VBIOS would need a VM 312 ROM-dump boot | not enumerated; and moot — the ACR row in §5.2 and §3 |
| strix / NAS | documented as unreachable from this lane (`docs/nv-r1-host-renderer.md` §1.1: strix answers ping but not a key; NAS copy is `0770 sarah:sarah`) | nothing |

Method note: file-identity comparisons use the **git blob sha** GitHub returns
for `src/nvidia/generated/` at each tag (one API call per tag), so "unchanged
between tags" is a property of the repository, not of my download. Artifact
hashes below are computed locally from the decoded arrays.

---

## 2. The GB202 artifact corpus, decoded

Every row was decoded from the tag's generated bindata with
`decode_bindata.py` (this card's tool, attached): it parses the `_data[]` array
(either `NvU8` byte literals or `NvU64` `0x…ULL` literals, including the
`NV_DECLARE_ALIGNED` form), trims to the declared `DATA SIZE`, inflates a
`DESC` with raw deflate, and parses `RM_RISCV_UCODE_DESC`.

### 2.1 FMC images (`g_gsp_gb20x_dyn_prd_gfw_image.bin`, in `GspRmFmcGfwProdSigned_GB202`)

| tag(s) | image B | image sha256 | header `+4` / `+0x0C` | `hash==SHA384(image)` | ever booted on GB202? |
|---|---|---|---|---|---|
| 570.86.16 | 198,984 | `eccbd503ae2f803cf2b523a15c98b14e97c5c1fe3928ed9b096dc8ce4178d3b2` | `0x30A00` / `0x25700` | true | no |
| 570.144 | 199,240 | `e4d176500d82d824c1a915202aca89e093c27febf8be1cf1a0705b544c2d6ddb` | `0x30A00` / `0x25800` | true | **yes — 19+ boots, 0x0B** |
| 570.211.01 | 199,240 | `52fb09e72c0d8300c294fa74e9be6d68e0bd2fcea9fa9d8b6deceefcde6d6515` | `0x30A00` / `0x25800` | true | no |
| 575.64.05 | 199,240 | `2e8cd01deb31b5238866da97c5bb26452407247ee17ed0b2db5cd9df7c657a0f` | `0x30A00` / `0x25800` | true | no |
| 580.65.06 | 215,624 | `c62856e9787222fc68c1c41e44e4ba7c022f9aaff3a56622f3437e3382af50ee` | `0x34A00` / `0x25800` | true | no |
| 580.178.04 = 580.196.11 | 215,624 | `cea2190cc8d7f74f65c0bd3dbf1c87377b2360417d866dd24345f9419b76703e` | `0x34A00` / `0x25800` | true | **twice: `rm23`/mode 13, pre-FRTS COT → `0x5`; `rm65`/mode 65, the `0x0B` COT → `0x0B` (§13)** |
| 590.48.01 | 286,792 | `6252aafcdd1a6e45dcce8beb6f7f7475ed706d470e57dca56728f9f41efaf0bf` | `0x46000` / `0x36E00` | true | no |
| 595.104.02 = 595.112.13 | 287,304 | `d7174df2c31d9e7f732fcc6176de233e8716dbc6e002be99bfcab497a986ed6a` | `0x46200` / `0x37000` | true | no |
| 610.57.04 | 287,304 | `17a15ce8f0e697f83d6d59ee48dc7beb9f8574ad6206d6956564c33fe5584ce0` | `0x46200` / `0x37000` | true | no |
| 615.78.08 | 287,304 | `5b74fefd42fc7b5f2e30c03e7558631d76899d9c6237ee9115c4e0829c63548b` | `0x46200` / `0x37000` | true | no |
| 620.70.06 = 620.70.10 = 620.73.04 | 287,304 | `55042efa5e02ded619105c80ae8b110d54ab5c3af22702419c8b0057bc19eef6` | `0x46200` / `0x37000` | true | **yes — 3 boots (`fh620`, `fh60b`, `fh62`), 0x824** |

All eleven carry the **same production public key**, `048bf118bbeb9c0f…`, and
every `UCODE_HASH` equals `SHA-384(image)` (the FMC is the only GB202 archive
that has a published hash). The header's `+0x0C` word is the **monitor code
size** and `+4` the image's total extent; they must agree with the paired boot
image's `RM_RISCV_UCODE_DESC` (§2.4), which is a free pairing check.

### 2.2 Boot images (`g_gsp_gb20x_dyn_prd_image.bin`, in `GspRmBoot_GB202`)

| tag(s) | image B | image sha256 | desc | code / data / manifest | ever staged on GB202? |
|---|---|---|---|---|---|
| 570.86.16 | 200,704 | `c3016bcff080784d1567029ef15128c2c41a7db4b1701e729685cf9a9a4a459b` | v5 (84/28) | `0xB200` / `0x25700`, data `0xA00` | no |
| 570.144 | 200,704 | `8b8cf1a1067c6f9d0cd966ca9553f9a6894a71b69607171a4c2c98e62e5ead4b` | v5 (84/28) | `0xB200` / `0x25800`, data `0xA00` | no (this is the **bootloader** geometry; `BLB.BIN` wraps it) |
| 570.211.01 | 200,704 | `df2ed9060e1ea253812afb183504a1088d728b3c81e67d44d0bd764e4174ba67` | v5 | `0xB200` / `0x25800`, data `0xA00` | no |
| 575.64.05 | 200,704 | `ec081adc7fef0db76f28e3a7dada13b60db542429187896ccb0970ee163ddc38` | v5 | `0xB200` / `0x25800`, data `0xA00` | no |
| 580.65.06 | 217,088 | `2fe1c6c232a014293cd92ccd6d43bc100b95a8cec36481924aa9097d84285e6c` | v5 | `0xF200` / `0x25800`, data `0xA00` | no |
| 580.178.04 = 580.196.11 | 217,088 | `ac0688326c1ea8aae2da26d785e7091bedb9a45565b03543a479ed05db0ba367` | v5 | `0xF200` / `0x25800`, data `0xA00` | **yes — every wall boot staged this file as `BOOTIMG.BIN`, and `rm65` staged it with the 580 FMC it belongs to** |
| 590.48.01 | 286,720 | `e1265e10912cc87a77e91944d162dd43294576e1ec0c545a03851831607652de` | v6 (92/28) | `0xF200` / `0x36E00`, data `0xA00` | no |
| 595.104.02 = 595.112.13 | 290,816 | `eb8d7c1f5ccbcff006a3f68031e973e2cca0853eeba5c4cbcfbc89dba8f36cfe` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 610.57.04 | 290,816 | `146a49168cae2b385d8ae7a79fd11c503004c070550dbca651967bddee0ed0c2` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 615.78.08 | 290,816 | `37159b4b7ce28066bbf83d11d50abff8e26722eb6bc2825290c14944a7c6aef0` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 620.70.06 = 620.70.10 = 620.73.04 | 290,816 | `fc42d2683f131df161c3afb1fcf9895b9fca8661e86234f3283fdcaacea3c4c1` | v7 (96/29) | `0xF200` / `0x37000`, data `0xA00` | **yes — `fh62` staged it as `BOOT620.BIN`** |

Two facts that correct a loose reading of the earlier rounds:

* **`BLB.BIN` is the 570.144 bootloader container**: its `nvfw` header is
  `magic 0x10de, ver 1, size 0x31100, hdr 0x18, data 0x6c + 0x31000`, and the
  200,704-byte image at `0x6c` is exactly the 570.144 `GspRmBoot_GB202`
  payload (`8b8cf1a1…`, full-image match checked). Its inner desc is the one
  the kernel quotes (`code@0xB200 size 0x25800`), so the 570-generation archive
  is the **bootloader image**, while 580+ ships the **GSP-RM boot image** at
  `code@0xF200`.
* `rm55`'s `0x1C` is therefore *consistent with* the 570 bootloader container
  being offered where a GSP-RM boot image was expected; the row does not by
  itself prove which check wrote `0x1C` (the same code answers descriptor-field
  violations), so this is an inference, not a second measurement.

### 2.3 Provenance correction: which driver ships which boot image

This is new and it changes a label, not a row. **The load-bearing check is the
staged file's own sha256, not a fingerprint inside the module:** `fw/BOOTIMG.BIN`,
the file every wall boot handed the FSP, is `ac0688326c1ea8aa…`, which is the
**580.178.04** boot image (§10). A module search alone cannot establish which
image a driver "ships": a 16-byte search finds the **container-layout** copy
whose first `0xA00` bytes are the manifest and whose remainder is *not* the
image, and it is easy to mistake that header match for body identity.

Measured on both closed modules (`nv-kernel.o_binary`, 570.144 = 98,164,824 B,
580.178.04 = 112,677,456 B), each image searched by its first 16 bytes, every
hit then compared **whole**:

| image | module | container-layout copy (header only) | byte-identical copy |
|---|---|---|---|
| boot `8b8cf1a1…` 570.144 (200,704, code `0xB200`) | 570.144 | @`0x2dbaea8` — manifest `0…0xA00` equal, diverges at `+0xA00` (module zeros) | **@`0x2f6f0e0`**, 200,704/200,704 B equal |
| boot `ac068832…` 580.178.04 (217,088, code `0xF200`) | 580.178.04 | @`0x36df1c8` — manifest equal, diverges at `+0xA00` | **@`0x3a17840`**, 217,088/217,088 B equal |
| FMC `e4d17650…` 570.144 | 570.144 | @`0x2e14800` — diverges at `+0x48` | **@`0x2d8ae60`** |
| FMC `cea2190c…` 580.178.04 | 580.178.04 | @`0x379cd60` — diverges at `+0x48` | **@`0x36ab180`** |

and neither module contains the other generation's image at all (0 hits). So the
**217,088-byte `BOOTIMG.BIN` that every wall boot staged is the 580.178.04
image** — a byte-identical copy of it is in the 580 module and none of it is in
the 570 one — and the 570.144 driver's own boot image (200,704, `8b8cf1a1…`,
`code@0xB200`) **has never been on the card**. The kernel comment at
`src/kernel/nv_fsp_cot.c` ("the vendor's dyn_prd image (0x34A00 content,
code@0xF200) the 570 driver ships") is **not supported by the bytes**: the 570
module embeds the 0x30A00 image, the 580 module the 0x34A00 one. No measured row
changes — the rows already name the size and the container — but the earlier
reports' phrase "the 570 boot image" for the 217,088-byte file is a
mislabelling, and it means the **true 570.144 pair has never been run** (§6,
rank 5).

### 2.4 The static pairing rule (new, zero-boot)

The FMC image header's `+0x0C` code size equals the paired boot image's
`monitorCodeSize` in **every** generation:

| class | FMC `+0x0C` | boot desc `monitorCodeSize` | tags |
|---|---|---|---|
| 199 KiB | `0x25700` / `0x25800` | `0x25700` / `0x25800` | 570.86 / 570.144–575 |
| 215 KiB | `0x25800` | `0x25800`, code at `0xF200` | 580 |
| 286 KiB | `0x36E00` | `0x36E00` | 590 |
| 287 KiB | `0x37000` | `0x37000` | 595, 610, 615, 620 |

So a *legitimate* one-variable FMC swap also stages that FMC's own boot image
(the `fh62` pair already did this for 620 and still got `0x824`; the geometry was
right, which is why the negative is informative).

---

## 3. "Newer than 570 and the 620.70.10 set": the proof of nothing new

The public tag list (`/tags?per_page=100`, two pages, down to `550.40.85` —
older than any GB202-capable tag) was read whole.
The GB202-relevant tags, their commit dates, and the git blob shas of the two
GB202 bindata:

| tag | commit date | `GspRmBoot_GB202` blob | `GspRmFmcGfwProdSigned_GB202` blob |
|---|---|---|---|
| 570.86.16 | 2025-01-30 | `631d347748a4` | `7206a8bdb750` |
| 570.144 | — | `a58b9899ef21` | `af135475fd87` |
| 570.211.01 | 2026-01-13 | `0c6f6c44655a` | `be18c302217c` |
| 575.64.05 | 2025-07-22 | `f0e397e7c9ab` | `7d904fa5b3a7` |
| 580.65.06 | — | `73158c175249` | `1d00cc123f67` |
| 580.178.04 | 2026-08-03 | `2cb97d9fe513` | `f0bc104f4c4d` |
| 580.196.11 | — | `2cb97d9fe513` | `f0bc104f4c4d` (same) |
| 590.48.01 | 2025-12-18 | `99e7e8facb41` | `9d4d3c4078b5` |
| 595.104.02 | 2026-09-22 | `3ec9002e1cf0` | `af6cedb07d44` |
| 595.112.13 | — | `3ec9002e1cf0` | `af6cedb07d44` (same) |
| 610.57.04 | 2026-08-03 | `20c3a76fbe04` | `03b00601bb8d` |
| 615.78.08 | 2026-10-07 | `2b09b1e7eb9f` | `8d978b7e4a64` |
| 620.70.06 | 2026-11-05 | `e64a54452c90` | `5ae49fb706e9` |
| 620.70.10 | 2026-11-23 | `e64a54452c90` | `5ae49fb706e9` (same) |
| **620.73.04** | 2026-11-18 (tag commit) | **`e64a54452c90`** | **`5ae49fb706e9`** (same) |

**The newest tag moves no GB202 byte.** `620.70.06`, `620.70.10` and `620.73.04`
share both blobs; the decoded images are identical (`55042efa…`, `fc42d268…`),
and the local 620.70.10 files are byte-identical to the 620.73.04 downloads
(sha256 `f36554a84612654e…` and `1053c2dc1127ff2a…`). Two other pairs are also
identical and cannot be re-booted as if new: `580.178.04 == 580.196.11`,
`595.104.02 == 595.112.13`.

Also checked at `620.73.04` (`src/nvidia/generated/`, 546 entries, one API read):

* **No `BooterLoadUcode` / `BooterUnloadUcode` / `Sec2Bl` for GB100 or GB202.**
  The booter set is `{TU102, TU116, GA100, GA102, AD102}`; `ksec2GetBinArchiveBlUcode`
  is TU102-only. The same is true of the 14 other tags' listings (`grep` over all
  15 `gen/*.json`: **0** GB-class booter files in every one).
* **No GB202 FSP image.** The only `fsp` entries are `g_kern_fsp_nvoc.c`,
  `g_fsp_events_nvoc.c` and `g_bindata_spdmGetBinArchiveFspL1Certificate_IMPL.c`.
  `kern_fsp_gb202.c` (620.73.04, and byte-identical in 620.70.10) still says:
  *"In GB202, Bootfsm triggers FSP execution out of chip reset."*

That is the complete answer to "anything newer": **the FMC/boot/ACR corpus is
closed at 620.70.10 for GB202**. What remains unmeasured is a *class*, not a
"newer" artifact: `590.48.01` (the only image class between the two measured
ends), its 287 KiB neighbours `595`/`610`, and the true 570.144 same-tag pair
(§6).

---

## 4. The 620 `UCODE_DESC_PROD`, re-decoded (confirming `t_320e84e5`)

The 620.70.10 boot image's desc is **not** a 32-byte tag: it is 4 `NvU64`
literals holding a **29-byte raw-deflate stream** of a **96-byte
`RM_RISCV_UCODE_DESC` version 7**. This run's decoder reproduces the parent's
result from both 620 tags, byte for byte:

```
stored 96 B (compressed size 29), inflated 96
+0x00 07 00 00 00   version 7
+0x20 00 0A 00 00   manifestOffset 0      (manifestSize 0xA00 at +0x24)
+0x28 00 0A 00 00   monitorDataOffset 0xA00
+0x2C E8 00 00 00   monitorDataSize 0xE800 (59,392)
+0x30 F2 00 00 00   monitorCodeOffset 0xF200
+0x34 70 03 00 00   monitorCodeSize 0x37000 (225,280)
+0x38 01 00 00 00   bIsMonitorEnabled 1
```

Image 290,816 B (`0x47000`): manifest `0 / 0xA00`, monitor data `0xA00 /
0xE800`, monitor code `0xF200 / 0x37000`, then 3,584 B of zeros — the three
regions tile the front of the file (quoted from `t_320e84e5` §1.2, re-checked
against the image here). The **570 desc is the same three offsets** with
`monitorCodeSize 0x25800`; what changes at 620 is the *size* of the image and the
struct's trailing fields (`bIsSmp`, `bIsPlicEnabled`, `numDebugBuffers`), not the
placement the FMC consumes. `kgspPopulateWprMeta_GH100` reads exactly
`monitorCode/Data/manifestOffset`, which is why the earlier "unpublished offsets"
worry was a decoder problem, not a missing field.

---

## 5. The two abort points, and which artifacts can move each

### 5.1 What the two points are, quoted

**`0x0B` — the 570.144 FMC's terminal code** (`t_0936a17f` `rm41`, GB202 VM 313):

```
[NV-FSPCOT] ttl t_s=27 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 statq_d0=0x0000000000000000
[NV-FSPCOT] ttl t_s=28 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 statq_d0=0x0000000000000000
```

**`0x824` — the 620.70.10 FMC's code** (`t_320e84e5` `fh60b`, GB202 VM 313):

```
fmc image bytes=287304  file=FMC620.BIN fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000012000824 … variant=1 partition=1 err=0x0000000000000824 info=0
```

`fh60b` staged the 217,088-byte boot image the wall always used (the 580.178.04
image, `ac068832…`; `t_320e84e5` calls it the 570 image — see §2.3) with the 620
FMC; `fh62` staged the 620 boot image with it. Both: `0x824` at 150 ms,
`0x14DC = 0x12000824`. So the boot image does not select this code either.

### 5.2 The rule the corpus establishes

> **With the host state equal in shape and in every field the host writes, the
> FMC image selects the abort point. The boot image, the GSP-RM signature, the
> radix3 ELF and the ACR are all downstream of, or absent from, that choice.**

"Equal in shape" is the honest form of this document's earlier "byte-identical",
which was false for the rows it cited and is withdrawn. Two things necessarily
move between any two boots and are not inputs:

* **the sysmem addresses.** The COT's three buffers and the boot image are
  allocated by size, so a different image size (or just a different boot) returns
  different addresses. Measured on GB202 VM 313:

  | FMC on the card | image B | `gspFmcSysmemOffset` / `frtsSysmemOffset` / `gspBootArgsSysmemOffset` |
  |---|---|---|
  | 570.144 (`fh58`, `rm41`) | 199,240 | `0x20C60E000` / `0x20C63F000` / `0x20C73F000` |
  | 580.178.04 (`rm65`) | 215,624 | `0x20C616000` / `0x20C64B000` / `0x20C74B000` |
  | 615.78.08 (`rm63`/`rm64`) | 287,304 | `0x20C65E000` / `0x20C6A5000` / `0x20C7A5000` |
  | 620.70.10 (`fh60b`) | 287,304 | `0x20C63A000` / `0x20C681000` / `0x20C781000` |

  The 615 and 620 rows make the point: same image size and same boot-image size,
  **different addresses in all three fields**, and the same `0x824`;
* **the bootloader geometry**, when the FMC changes *class*: the FMC header's
  `+0x0C` code size must equal the paired boot desc's `monitorCodeSize` (§2.4),
  so 570→615 also moves `sizeOfBootloader` `0x35000`→`0x47000`, the descriptor's
  `bootimg` bus address and the COT addresses (§11.1). The boot-image axis is
  controlled separately: `fh62` (620 FMC + 620 image) is counter-identical to
  `fh60b` (620 FMC + the 580 image), and `rm56` (570 image address unmapped)
  still ends `0x0B`.

What is genuinely held fixed across a pair is what the host *writes*: the COT
shape (`frtsVidmemOffset` `0x4000000` / size `0x100000`), the `GSP_FMC_BOOT_PARAMS`
page, the `WprMeta` size/heap fields, and the GSP-RM image/signature.

Evidence for each clause:

| artifact | can it move `0x0B`? | can it move `0x824`? | the row |
|---|---|---|---|
| **FMC image** | **yes** — 570.144 and 580.178.04 are `0x0B`; 615.78.08 and 620.70.10 are `0x824` | **yes** | `rm41`, `fh58`, `rm65`, `rm63`, `fh60b` |
| **boot image** **content/address** | no | no | `rm56` (address unmapped → same `0x0B`); `fh62` (620 image → same `0x824`) |
| **boot image** **container form** | no — it changes a *different* code | no | `rm55` (`BLB.BIN` → `0x1C`) |
| GSP-RM **signature** | no | no | `rm53` (`0x1D`) |
| radix3 **ELF** | no | no | `rm54` (`0x02`) |
| **ACR / SEC2** | no — no blob exists, no window accepts one | no | `t_67f09feb`: `0x840000` = `0xBADF4100` 7/7, gate `0xAD00BC` = `0xFF` |
| **FSP** | no — in-chip, BootFSM-started, not stageable | no | `kern_fsp_gb202.c`, no FSP image in any tag |
| descriptor / COT **fields** | no (each measured) | no (each measured) | `docs/gb202-rm-parked.md` §5, 20 rows |

### 5.3 The gap that is left in the corpus

**Four** of the eleven FMC images have been on the card:

* `570.144` → `0x0B` (19+ boots, the whole `t_a769beaa`/`t_0936a17f` ladder);
* `580.178.04` → `0x5` once (`rm23`, mode 13, vendor FMC + vendor boot image,
  before `rm29` found that the COT needs a non-zero FRTS pair), and **`0x0B`**
  once on the corrected COT (`rm65`, mode 65, §13);
* `615.78.08` → `0x824` (3 boots: `rm63`, `rm63b`, `rm64`);
* `620.70.10` → `0x824` (3 boots: `fh620`, `fh60b`, `fh62`).

Measured, the class rule is: **199–215 KiB → `0x0B`, with WPR2 armed at
`0x7E5A200/0x7F1EE00` under the vendor heap pair; 287 KiB → `0x824` at 150 ms,
with WPR2 never armed.** What is left unmeasured:

* **`590.48.01` (286,792 B, `0x36E00`) — the only class member between the two
  measured ends** (§6 rank 1);
* `595.104.02` and `610.57.04` — 287 KiB, the same class as 615/620;
* `580.65.06` — the same 215 KiB class **and the same `0x25800` code size** as
  the measured `580.178.04`, so it cannot answer anything `rm65` did not;
* `570.86.16`, `570.211.01`, `575.64.05` — the 199 KiB class, same geometry as
  `570.144` (`0x86.16` differs only in its `0x25700` code size);
* the **true 570.144 same-tag pair** (§6 rank 3) — a boot-image control, not a
  class question.

---

## 6. Ranked candidates, with the boot each costs

A candidate is worth a boot only if it can change one of the two codes at fixed
host inputs. Ranking is by *new information per boot*, not by version. **Ranks 1
and 2 of the survey version are spent** — `615.78.08` (§11, `0x824`) and
`580.178.04` (§13, `0x0B`) — so the table below is the reconciled ranking after
both, and §13 spends rank 1.

| # | candidate | pair to stage | boots | which abort it can move | why it is ranked here |
|---|---|---|---|---|---|
| 1 | **590.48.01** | FMC `6252aafc…` (286,792 B) + its own `GspRmBoot` `e1265e10…` (286,720 B, `0xF200`/`0x36E00`) | **1** | `0x824` or `0x0B` | **the only class member between the two measured ends**: 199/215 KiB answers `0x0B` with WPR2 armed (`fh58`, `rm65`), 287 KiB answers `0x824` with WPR2 never armed (`rm63`, `fh60b`). A 286 KiB build decides whether the abort arrived with the first `0x36E00` image class or only with the later 287 KiB builds |
| 2 | **610.57.04** | FMC `17a15ce8…` + `BOOT610` `146a4916…` (290,816 B, `0xF200`/`0x37000`) | 1 | re-measures `0x824` | an earlier 287 KiB build (2026-08-03); informative **only if 590 answers `0x0B`**, where it would place the 286→287 transition inside the class |
| 3 | **570.144 same-tag pair** | FMC `e4d17650…` + the **570.144** boot `8b8cf1a1…`, `code@0xB200` | 1 | `0x0B` (control) | the true 570 driver pair has **never** been on the card (§2.3): every wall boot used the 580 boot image at `0xF200`. Expected to reproduce `0x0B` (`rm56` says the boot image is read after that point), so it is a control, not a lever; it needs the boot image and the `bootloaderCodeOffset` changed together |
| 4 | **580.65.06** | FMC `c62856e9…` (215,624 B) + `BOOT580.65` `2fe1c6c2…` (217,088 B, `0xF200`/`0x25800`) | 1 | nothing new | same 215 KiB class **and the same `0x25800` code size** as the measured `580.178.04`; it can only re-measure that class. **Do not spend** — this is the pair §12.5 used to name, and the naming was wrong on three counts (see §12.5) |
| 5 | 570.86.16, 570.211.01, 575.64.05, 595.104.02 | — | — | — | same class and geometry as a measured member (199 KiB with `0x25800`, or 287 KiB with `0x37000`); no question left that rows 1–2 do not answer |

**Already spent (do not re-run):** 615.78.08 (§11, `0x824`), 580.178.04
(§13, `0x0B`), 620.70.10 (`t_320e84e5`), 570.144 (the `0x0B` ladder).

**Do not spend:** 620.70.06 / 620.70.10 / 620.73.04 (identical to the FMC already
booted), 580.196.11 (= 580.178.04), 595.112.13 (= 595.104.02), any "newer" FMC
(none exists), any booter/ACR blob (none exists), any FSP (not stageable), and
every host-field variation in `docs/gb202-rm-parked.md` §5.

---

## 7. The single comparison that would distinguish them

### 7.1 Primary: one variable at a time, and the one class point left

Hold the **host state** — the COT shape (FRTS-in-FB 64 MiB/1 MiB, `frts_size`
0), the boot-args page (`GSP_FMC_BOOT_PARAMS`), the descriptor's size/heap
fields and the GSP-RM image/signature — equal to the state that yields `0x0B`
(mode 58/59's vendor heap pair), and vary **only the FMC generation**. A
generation is an FMC image **plus its paired boot image**: the FMC header's
`+0x0C` code size must equal the boot desc's `monitorCodeSize` (§2.4), so the
pair moves together and some descriptor bytes necessarily move with it — for
570→615 that is `sizeOfBootloader` `0x35000`→`0x47000`, the descriptor's
`bootimg` bus address, and all three COT sysmem addresses (§11.1). The
boot-image axis is controlled separately and does not need to ride along:
`fh62` (620 FMC with the 620 boot image) is counter-identical to `fh60b` (620
FMC with the 580 image), and `rm56` (570 image address unmapped) still ends
`0x0B`. The corpus now has **three measured class points**:

* 570.144 (199,240 B) → `0x0B`, WPR2 armed (19+ boots, `fh58` the clean control);
* 580.178.04 (215,624 B) → `0x0B`, WPR2 armed at the same pair (`rm65`, §13);
* 615.78.08 / 620.70.10 (287,304 B) → `0x824` at 150 ms, WPR2 never armed
  (`rm63`, `fh60b`).

The missing point is **590.48.01** — 286,792 B, code size `0x36E00`, the only
image class between the `0x0B` end and the `0x824` end, with its own
`GspRmBoot` (`e1265e10…`, 286,720 B, `0xF200`/`0x36E00`). One boot decides:

| 590 outcome | what it proves |
|---|---|
| `0x824`, `0x14DC` formatted | the abort arrived with the **first `0x36E00` class** (590), not with the 287 KiB builds; the class line is the FMC image size/geometry, and 595/610 need no boot |
| `0x0B`, WPR2 armed | the abort arrived **inside** the 286→287 KiB step; 610.57.04 (§6 rank 2) then places the transition, since 610 and 615 share the 287 KiB class |
| anything else (a new code, or `0x0`/`INIT_DONE`) | the class story is incomplete or the wall moved — an **R2** event if `statq_written=1 linked=1 initdone=1` |

**What it needs:** the HobbyOS 5090 image in **VM 313**, booted with
`tools/nv/gb202fsp-boot.sh <img> <tag>` (one boot, `STOP_AFTER_VERDICT=1`,
`STOP_PATTERN='^\[NV-FSPRM\] verdict='`), the 590 FMC container staged as
`/NVIDIA/GSP/FMC590.BIN` with its own boot image as `/NVIDIA/GSP/BOOT590.BIN`
(the kernel's mode 65 pattern: one new `fmc_name` + the default `BOOTIMG.BIN`
geometry replaced by `0xF200`/`0x36E00`; `FMC.BIN`/`BOOTIMG.BIN` stay staged for
the older modes). **No kernel module is involved** — our kernel is the driver.
The §8 readbacks are already in the sampler (§13), so nothing else is needed
before the boot.

**Guest/module flavour for the control (7.2):** the Linux guest is **VM 301**
(Ubuntu 24.04, DKMS), and the module must be **`nvidia-open-580.178.04`** — the
closed `nvidia.ko` **refuses a GB202 by design**, measured 2026-11-27:
`NVRM: … requires use of the NVIDIA open kernel modules`, `RmInitAdapter failed!
(0x22:0x56:897)` (`docs/nv-r1-host-renderer.md` §1.1). A 5090 baseline built on
the closed module fails before it can say anything about the FSP.

### 7.2 The control only NVIDIA can give: its own path on the same part

The primary comparison can tell you *where* the abort changes; it cannot tell you
whether **any** of our FMCs is the one this part expects. The only success path
on GB202 is NVIDIA's own driver, and the comparison is a single guest boot:

* **guest:** VM 301, Ubuntu 24.04, `41:00` passed through (`qm set 301 --hostpci0
  0000:41:00,pcie=1`), `nvidia-open-580.178.04` (not the closed module);
* **capture, at the same offsets this kernel reads:** `NV_PGSP_FALCON_MAILBOX0/1`,
  `NV_PBUS_ZB_SW_SCRATCH_GSP_FMC_ERROR` (`0x14DC`), `NV_THERM_I2CS_SCRATCH`
  (`0xAD00BC`), and `NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0..3)`, on the success
  path and on any failure;
* **the known answer to "which FMC":** the 580 open module binds
  `GspRmFmcGfwProdSigned_GB202` = **`cea2190c…` (215,624 B)** — the image §13
  staged and measured (`0x0B`, WPR2 armed). So the "is a 215 KiB FMC able to run
  on this part at all" half is already answered **yes** (§13.3): the FMC executes
  and the FSP lays out WPR2. What the control still decides is the other half —
  whether **NVIDIA's own host structures** get past `0x0B` with that image. If
  they do, the remaining difference is in the COT/descriptor/args our kernel
  writes; if they do not, the wall is the in-chip FSP's generation and no host
  artifact can move it.

That is the distinguishing experiment, and it is one guest boot plus one HobbyOS
boot against a **known FMC identity**, not another sweep of host fields.

---

## 8. The instrument gap (done, 0 boots)

**Status: closed in §11 and reused in §13.** The six readbacks below are in the
sampler from mode 63 on, so every mode-58/63/64/65 boot prints
`SCRATCH_GROUP_2(0..3)`, `PGSP_MAILBOX(1..3)` and `0x14DC`: they read
`0x9F,0,0,0xA` / `0,0,0` / `0` in `rm65` and were frozen through the `0x0B`
transition (§13.2). The history of the gap is kept because it says *why* the
loop reads what it reads.

NVIDIA's own failure path, `kfspDumpDebugState_GB100` (620.73.04
`src/nvidia/src/kernel/gpu/fsp/arch/blackwell/kern_fsp_gb100.c`), reads **more
registers than this kernel's sampler**:

```
NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0) … (3)
NV_PGSP_FALCON_MAILBOX0 / MAILBOX1
NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR                     (0x14DC)
NV_PGSP_MAILBOX(0) … NV_PGSP_MAILBOX(NV_PGSP_MAILBOX__SIZE_1-1)
FSP microcode version (GFW FSP_UCODE_VERSION), then the CMS2 and DMEM logs
```

Our sampler prints `m0`, `m1`, `0x14DC`, `HWCFG2`, `FALCON_OS/RM`, `rvctl` — but
**not** `SCRATCH_GROUP_2[0..3]` and **not** `NV_PGSP_MAILBOX(1..)`: the kernel
reads `NV_GSP_ENGINE_MAILBOX0` (`0x804`, index 0) but never the rest of that
array. `t_67f09feb` measured the FSP's own scratch at +2 ms
(`scratch2 s0=0x9F s3=0xA`, `ucode_ver 0x44C`) — those are live FSP progress
words, and the `0x0B` transition has never been sampled against them. Adding six
readbacks to the 1 Hz loop costs **0 boots** and is the cheapest chance to give
`0x0B` a name (the FMC's code is encrypted; the FSP's scratch is not).

---

## 9. Statically ruled out — do not spend a boot on it

Everything in `docs/gb202-rm-parked.md` §5 still holds (descTarget, `fbSize`,
`frtsSize`, signature/ELF/boot-image identity and address, the boot-image
container, FRTS-in-FB, the heap pair, the 620-pair result, `0x14DC` from a 570
FMC). This survey adds:

| hypothesis | ruled out by |
|---|---|
| an FMC **newer than 620.70.10** exists | `620.73.04` is the newest tag; its two GB202 blobs are identical to `620.70.10`'s (§3) |
| `620.70.06` is a different GB202 artifact | same two blob shas as 620.70.10/620.73.04 |
| `580.196.11` / `595.112.13` are new artifacts | same blob shas as 580.178.04 / 595.104.02 |
| a GB202 **SEC2 booter / ACR ucode** is reachable | absent from 15/15 `generated/` listings (442→546 entries), absent from linux-firmware (`nvidia/gb202/` has `gsp/` only), absent from both `.run`s |
| "the 217,088-byte boot image is the 570.144 one" | the 570.144 `nv-kernel.o_binary` embeds `8b8cf1a1…` (200,704, code `0xB200`); `ac068832…` is in the 580.178.04 module (§2.3) |
| a GB202 **FSP** image is stageable | absent from every tag; `kern_fsp_gb202.c`: BootFSM starts it out of chip reset; the gate `0xAD00BC` is the host's only lever |
| the **boot image** can move `0x824` | `fh62` (620 image, right geometry) == `fh60b` (570 image) == `0x824` |
| the **boot image** can move `0x0B` | `rm56` (address unmapped) == baseline `0x0B` at the same time |
| **`0x14DC`** is readable from a 570 FMC | 0 in all 8 `t_0936a17f` boots; the formatted-error reader/format is 610+/620 |
| a **cross-generation FMC pair** is invalid by geometry | the FMC header `+0x0C` matches the boot desc's code size for every class (§2.4); 570 FMC + 580 boot is code-size-compatible (`0x25800`) |
| the **ACR writes/copies anything** into FB | `rm43`/`rm59b` census: `other=0 meta=0 gfw=0`; 256-byte stride over the WPR2 band, `nz_head=0 nz_all=0` |
| "**`0x824` is 620-specific**" | `rm63`/`rm63b`: 615.78.08 + its own boot image, `cot_error=0`, `m0=0x824`, `0x14DC=0x12000824` at 150 ms (§11.3) |
| the **615 boot image** can move `0x824` | the 615 FMC with the 615 image == the 620 FMC with the 620 image == `0x824`; the boot image is not the variable in either |
| the FSP's **SCRATCH_GROUP_2 / PGSP_MAILBOX(1..3) / CMS2 log** names the `0x824` | `rm63b`: scratch frozen at `0x9F,0,0,0xA`; `PGSP_MAILBOX(1..3)=0`; CMS2 window 48/48 identical pre/post, no text (§11.4) |
| the vendor's 615-stock **`nonWprHeapSize = 0x300000`** moves `0x824` | `rm64`: same pair, same COT, `nonwpr_heap=0x300000` read back from the block, `m0=0x824` / `0x14DC=0x12000824` unchanged at all 13 samples (§12) |
| **`WprMeta.frtsSize = 0x100000`** moves it | not booted: decorative on this path per the corpus the `WprMeta` diff comes from (05.12 "the COT value wins") and our COT already carries 1 MiB (§11.6) |
| the **580.178.04 FMC** moves the code off `0x0B` | `rm65`: same host state, same boot image, WPR2 armed at `0x7E5A200/0x7F1EE00` (**identical** to `fh58`'s 570-FMC pair), `m0=0x0B` with the pair torn down (§13) |
| `0x824` is a property of "the FMC NVIDIA ships for this driver" | the 580.178.04 image is the one `nvidia-open-580.178.04` binds for GB202 (§2.3, §7.2) and it answers `0x0B`, not `0x824` (§13) |
| the **215 KiB class** can reach the wall | `rm65`: WPR2 is armed exactly as with the 570 FMC and the attempt still terminates `0x0B`; `wprm-after untouched=yes` (§13) |

**Not** ruled out: the **590.48.01 class point** (§6 rank 1 — the only image
class between the two measured ends); **610.57.04** as its follow-up; the **true
570.144 same-tag pair** (§6 rank 3, a control); the **meaning of `0x0B`** (not in
`flcnretval.h`, not an FSP `kern_fsp_retval.h` code, and the producing FMC is
encrypted); and the only in-band namer, the **FSP DMEM log over the MCTP
download-log RPC** (`_kfspGatherDmemLog_GB100`). `0x824`'s meaning is likewise
not in the public map; §11.5 places it in the `WprMeta`-parser block. Both
`WprMeta` fields in which our block differed from the vendor's 615 stock are
answered — `nonWprHeapSize` by measurement (§12), `frtsSize` by the corpus
(§11.6).

---

## 10. Method and evidence

Static-only commands, reproducible from this workspace:

```sh
# the tag matrix (one API read per tag, blobs by raw.githubusercontent)
curl -s "https://api.github.com/repos/NVIDIA/open-gpu-kernel-modules/contents/src/nvidia/generated?ref=<tag>"
# the decode (no third-party deps; raw deflate for DESC, hashlib for identity)
python3 decode_bindata.py matrix/*_Boot.c matrix/*_FmcGfwProdSigned.c … > survey.json
# the FMC header geometry check
python3 fmchdr.py    # +4 = image extent, +0x0C = monitor code size
```

Attached to the card: `REPORT-gb202-rm-objects.md` (this file),
`gb202-rm-survey-evidence.tar.gz` (the 15 `gen/*.json` listings, `survey.json`,
the decoded FMC/boot images, `decode_bindata.py`, `fmchdr.py`, `mktable.py`,
`prov23.py`), the full `survey.json`, and for the three boot rounds
`gb202-rm-615-evidence.tar.gz` (§11: `rm63`/`rm63b` logs, `KEY-ROWS-615.txt`,
the mode-63 source diff), `gb202-rm-mode64-evidence.tar.gz` (§12: the `rm64`
log, `KEY-ROWS-64.txt`, the mode-64 source diff, `cmp6364.py`, `keyrows64.py`,
`check_rows12.py`, `prov23.py`, `SHA256SUMS`) and
`gb202-rm-mode65-evidence.tar.gz` (§13: the `rm65` log, the `fh58` control,
`KEY-ROWS-65.txt`, the mode-65 source diff and the pre-65 snapshot,
`keyrows65.py`, `build65.sh`, `SHA256SUMS`).

Files staged by the wall (for the boots in §11–§13), with this run's hashes:

```
FMC.BIN     199,820  cb59a35c1d4bd1274d7267fd…  (linux-firmware 570.144 container; image e4d17650…)
FMCV.BIN    216,204  6adbd92d63c375aa3dc40ea1…  (580.178.04 container; image cea2190c…)
FMC580.BIN  215,624  cea2190cc8d7f74f65c0bd3d…  (the 580.178.04 decoded image, not a container)
FMC595.BIN  287,884  17057676d36400fdeb7dda5e…  (595.104.02 bindata; image d7174df2…)
FMC620.BIN  287,884  ec87ba3509186302ab194f7a…  (620.70.10 container; image 55042efa…)
BOOTIMG.BIN 217,088  ac0688326c1ea8aae2da26d7…  (580.178.04 boot image — the one field's pair in §13)
BLB.BIN     200,812  d40b48e431d1707dc77af360…  (570.144 bootloader, nvfw container)
BOOT620.BIN 290,816  fc42d2683f131df161c3afb1…  (620.70.10 boot image)
```

**No boot was spent by the survey pass. Nothing on r1 was started, bound, or
installed by it.** Closing state then: VM 313 `stopped`, VM 300 `stopped`,
`pgrep -c qemu-system-x86` = 0 on r1 and on this worker.

---

## 11. The 615.78.08 boot — the one variable, measured (2 boots, 2026-11-27)

This is the boot §7.1 named, run after the review that asked for it. Part in every
row: **GB202, `41:00.0`, VM 313**. Harness `tools/nv/gb202fsp-boot.sh`, one boot
per named hypothesis, `STOP_AFTER_VERDICT=1`, `STOP_PATTERN='^\[NV-FSPRM\] verdict='`.
Two boots were spent: **`rm63`** (the named one-variable boot) and **`rm63b`**
(the same state, plus the FSP's own CMS2 window — hypothesis H2 in §11.4).

### 11.1 What was staged, and what necessarily moved with it

Both artifacts were decoded from the **`615.78.08`** tag's generated bindata
(`GspRmFmcGfwProdSigned_GB202`, `GspRmBoot_GB202`) by the survey's own decoder —
the same sources the §2 corpus table came from, not a re-download of anything new.

| staged file | bytes | sha256 | geometry |
|---|---|---|---|
| `/NVIDIA/GSP/FMC615.BIN` | 287,884 | `ae4e70559f14d9becf7f0c4014a990d0873d174f3af0cc4913484e2cfb3238d5` | container: hash@0x150 == SHA-384(image) ✔, pkey@0x1E0 = `048bf118bbeb9c0f…`, image 287,304 = `5b74fefd…` |
| `/NVIDIA/GSP/BOOT615.BIN` | 290,816 | `37159b4b7ce28066bbf83d11d50abff8e26722eb6bc2825290c14944a7c6aef0` | desc v6: manifest 0/0xA00, data 0xA00/0xE800, code 0xF200/0x37000 |

`FMC615.BIN` was built from the bindata's four blocks with the same
`mkfmc.py` shape the 620 container used (hash 48 / signature 96 / publickey 97 /
image at 0x244), and the container's hash section **equals `SHA-384(image)`** —
checked, not assumed. FMC image `+0x0C` = `0x37000`, the boot desc's
`monitorCodeSize` = `0x37000` (§2.4's pairing rule holds), so the FMC and its
own boot image are one *generation* and move together.

**What is held equal, and what did not have to move.** The GSP-RM image
(63,541,248 B), its 4,096-byte signature, the radix3 tree (33 pages), the boot
args page (`GSP_FMC_BOOT_PARAMS`), the FRTS sysmem buffer, the descriptor's heap
pair (`0x8700000` / `0x220000`), `frtsSize = 0` and the COT's
`frtsVidmemOffset = 0x4000000` / `frtsVidmemSize = 0x100000` are unchanged from
the `0x0B` state (mode 58/59). The FMC generation swap necessarily moved three
things, and the logs show them:

| field | `fh58` (570 pair) | `rm63`/`rm64` (615 pair) |
|---|---|---|
| `sizeOfBootloader` | `0x35000` | `0x47000` |
| descriptor `bootimg` bus address | `0x20C7C6000` | `0x20C82C000` |
| COT `gspFmcSysmemOffset` / `frtsSysmemOffset` / `gspBootArgsSysmemOffset` | `0x20C60E000` / `0x20C63F000` / `0x20C73F000` | `0x20C65E000` / `0x20C6A5000` / `0x20C7A5000` |

Those are consequences of a different pair of files (different sizes, different
allocation), not extra variables: the boot-image axis is controlled separately by
`fh62` vs `fh60b` (identical counters) and `rm56` (unmapped address, same
`0x0B`), and the descriptor's own fields (`code 0xF200`, `data 0xA00`,
`manifest 0`) are identical in both columns.

The earlier sentence here ("the COT, the WPR descriptor, the boot args page …
byte-identical") was wrong and is withdrawn: the descriptor's `bootimg` address
and size and the COT's three sysmem addresses changed with the pair.

### 11.2 The rows

The block was consumed by the FSP — the COT round trip is clean:

```
[NV-FSPRM] start gsp=rm mode=63[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63: FMC615.BIN,BOOT615.BIN)
fmc image bytes=287304 fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
bootimg bytes=290816[NV-FSPCOT]  file=BOOT615.BIN d0=0x000000001156E500 d1=0x000000003996718D d2=0x0000000045AF2B00 d3=0x00000000C74FBC4D code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] cot class=1[NV-FSPCOT] cot cmd_nvdm=20[NV-FSPCOT] cot errorCode=0
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
```

and the terminal state is already there at the first sample and never moves
(`rm63b`; `rm63` is identical — the CMS2 window exists only in `rm63b`):

```
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=63 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000824 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000012000824 radix3=1
[NV-FSPCOT] gsp-shape post off=0x0000000000110040 v=0x0000000000000824
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
```

All eleven samples (`t_ms` 150, 250, 400, 600, 1000, 3000, 6000, 10000, 15000,
21000, 27000) and the two extra ones (40000, 55000) carry the same
`m0=0x824`, `fmcerr=0x12000824`, `wpr2_lo=0x7FFFFE00`, `wpr2_hi=0` — the verdict
row is not a last sample, the state is constant for 55 s.

### 11.3 What this settles

`0x824` is **not 620-specific**. The immediate predecessor of the 620 branch, run
with **its own** boot image and a COT the FSP accepts (`errorCode=0`), emits the
identical `0x14DC = 0x12000824` (variant 1, partition 1, err `0x824`, info 0) and
dies before `t=150 ms`.

The measured corpus now has two families, and the boundary is a property of the
FMC image, not of the boot image, the COT, or the host structures:

| FMC class | boots | `MAILBOX0` | WPR2 at 150 ms | status queue |
|---|---|---|---|---|
| 570.144, 199,240 B | 19+ | `0x0B`, 0.2–28 s | **up** (`0x07E61200/0x07F1EE00` at 27 s, `rm41`) then torn down | never written |
| 615.78.08, 287,304 B | `rm63`, `rm63b` | **`0x824`** at 150 ms | **already down** (`0x7FFFFE00/0`) | never written |
| 620.70.10, 287,304 B | `fh620`, `fh60b`, `fh62` | **`0x824`** at 150 ms | already down | never written |

So the 287 KiB FMCs do not reach the state where the FSP arms WPR2 at all, while
the 570 FMC arms it, runs (the ACR reads nothing — `rm59b`), and then aborts with
`0x0B`. The wall is on the newer FMC's *entry* path.

### 11.4 The §8 instrument, closed — and negative (`rm63b`)

The readbacks §8 named were added before `rm63` (so both boots carry them) and
are in every sample above:
`NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0..3)` (`0x8F0320+4i`),
`NV_PGSP_MAILBOX(1..3)` (`0x110808/80C/810`; GB100 `__SIZE_1` = 4, so `gmb0` at
`0x804` was the only word this kernel had read), plus the FSP boot-complete gate
`NV_THERM_I2CS_SCRATCH` (`0xAD00BC`) — eight words where the kernel had read one.

* The FSP scratch is **frozen** at its boot-time values — `0x9F, 0, 0, 0xA` — the
  same four words `t_67f09feb` measured at +2 ms and the same the `[NV-PFSP]`
  probe prints before any COT. Neither the COT nor the `0x824` moves them.
* `i2cs = 0xFF` (FSP_BOOT_COMPLETE.SUCCESS) throughout; `PGSP_MAILBOX(1..3) = 0`.
* **H2: the FSP's CMS2 log.** The vendor's own `kfspDumpDebugState_GB100` reads
  it through `NV_POOBHUB_RCV_INDIRECT_CMS2_MEM_RD_ADDR` (`0x8AA374`) /
  `…_RD_DATA` (`0x8AA378`), words `0x50…0x7F`. Read pre-COT and again after 55 s:
  **48/48 dwords identical**, and the whole window only ever contains
  `{0, 0x7, 0xA, 0x65, 0x9F, 0xFF, 0x140000}` — a repeated 7-word status record
  (the same `0x9F`/`0xA` the scratch holds), **no text, and no new record for the
  abort**. The FSP does not write this failure anywhere the host can read.

(The CMS2 rows are printed with the `[NV-FSPCOT]` prefix repeated inside the row
because the helper prints one prefix per call; cosmetic, and the 48 values were
parsed and compared programmatically, not by eye.)

### 11.5 What `0x824` is, and is not

* **Not an FSP RPC error.** `kern_fsp_retval.h` (620.73.04) defines exactly
  `FSP_OK 0x00`, `FSP_ERR_IFS_ERR_INVALID_STATE 0x9E`, `…FILE_NOT_FOUND 0x9F`,
  `…NOT_SUPPORTED 0xA0`, `…INVALID_DATA 0xA1`, `…INVALID_KNOB_ID 0x1E3`.
  `0x824` is outside that set; `kfspErrorCode2NvStatusMap_GH100` would map it to
  `NV_ERR_GENERIC`. It is an FMC/ACR-side code reported in the *formatted*
  register (variant 1); the same value also lands in `MAILBOX0` (`0x824`), which
  the vendor calls an "effectively 8-bit" channel for older FMCs.
* **Not in the public map.** The GB202 reverse-engineering corpus
  (`shuttie/bf16-unlock-blackwell-research`, `specs/08_errors-and-logging.md`)
  maps seventeen boot codes — `0x827` (`WprMeta.magic`), `0x828` (`revision`),
  `0x82e` (`sizeOfBootloader` alignment), `0x836` (256 MiB bootloader),
  `0x83d/0x83e/0x841/0x885/0xa97/0xaaa/0xa0f/0xaff`, and the FSP responses
  `0xd6/0xd9/0x33/0x377/0x1db` — and **does not contain `0x824`**. Its numeric
  neighbours place it in the `WprMeta`-parser block, immediately before `magic`.
* **Our `WprMeta` differs from the vendor's 615 stock in two fields** (same
  corpus, GB202 PRO, 615.71.09, `specs/01_boot-chain.md#wprmeta`):
  `frtsSize` **0x100000** there vs **0** here, and `nonWprHeapSize` **0x300000**
  there vs **0x220000** here (`RM_NONHEAP_BYTES_GB202` = 2,228,224; the row
  `wprm sizes … nonwpr_heap=0x0000000000220000` is what was staged). `gspFwHeapSize`
  matches (`0x8700000`); `fbSize = 0` is documented as ignored on the 615 class
  (an `0xffffffff` value still boots), and `sizeOfBootloader = 0x47000`,
  `bootloader{Code,Data,Manifest}` `0xf200/0xa00/0`, `sizeOfSignature = 0x1000`
  all match our staged pair.

### 11.6 The named next boot — `nonWprHeapSize`, not `frtsSize`

The first version of this paragraph named `WprMeta.frtsSize = 0x100000` first.
**That is withdrawn**: the same corpus this document cites for the `WprMeta`
diff measured the field as decorative on this path —
`experiments/05_fmc-fsp-boot-inputs.md` §05.12 ("no cross-check …
`WprMeta.frtsSize` is decorative on this path. The COT value is authoritative")
and `specs/01_boot-chain.md` line 179 ("Disagreement with the COT is ignored;
the COT value wins") — and our COT already carries `frtsVidmemSize = 0x100000`,
so making the struct agree cannot change the layout under that rule. Nothing in
the `0x824` state suggests the 287 KiB class reverses a rule the corpus measured
on the same part, so no boot is spent on it.

The one measured difference left is **`nonWprHeapSize 0x220000 → 0x300000`**
(the corpus's "honoured 1:1" field). §12 spends that boot. After it, the next
boot is the survey's rank-2 pair — **580.178.04** (FMC `cea2190c…`, 215,624 B,
header `+0x0C` = `0x25800`; its own boot image is the `ac068832…` `BOOTIMG.BIN`
already staged) — which §13 spends; and then the **590.48.01 class point**
(§6 rank 1). The FSP's DMEM log over the MCTP download-log RPC
(`_kfspGatherDmemLog_GB100`) remains the only in-band namer, and it needs that
packet protocol implemented — not a COT or `WprMeta` field.

The VM 301 / `nvidia-open-580.178.04` control still needs coordinator sign-off
(§7.2) and was not run.

### 11.7 Acceptance

**Unmet, and no mock or shim value is used or claimed.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0` in both boots, and the status queue's producer header is 0 at every
sample (`[NV-RPC] peer … (header still zero)`). The part is GB202, `41:00.0`, VM
313 in every row.

---

## 12. The mode-64 boot — the last one-field difference, measured (1 boot, 2026-11-27)

§11.6 promoted one field and this section spends it: **`WprMeta.nonWprHeapSize`
`0x220000` → `0x300000`** (the vendor's 615 stock value), 615.78.08 pair, mode 58
host state, COT unchanged. One VM 313 boot, `tools/nv/gb202fsp-boot.sh`, tag
`rm64`, `STOP_AFTER_VERDICT=1 STOP_PATTERN='^\[NV-FSPRM\] verdict='`. The
kernel mode is `64`; its source diff is attached (`nv_fsp_cot.c.mode64.diff`).
No CMS2 window in this boot — §11.4 measured that instrument static and
textless, so it would only have added rows.

### 12.1 What moved, and what did not — the two logs diffed

Mode 64 is mode 63 plus one descriptor dword. The proof is mechanical: the two
serial logs are **586 lines each, 13 samples each**, every sampled register is
single-valued in each boot and **identical between them**
(`m0=0x824`, `fmcerr=0x12000824`, `wpr2_lo=0x7FFFFE00`, `wpr2_hi=0`,
`s2=0x9F,0,0,0xA`, `mb1..3=0`, `i2cs=0xFF`, `hwcfg2=0x8187A7F7`), and the
row-by-row diff of the whole GSP/FSP path is **one** row (`rm63`, then `rm64`):

```
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000220000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000300000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
```

plus the mode number in the two rows that print it, and boot-time noise that is
not an input: the boot timestamp, the image sha, the ptimer pair, the guest's
NVMe / `[BLKQ]` / `[NFSIO]` rows (their values are per-boot *timing* of the
block driver's own init — one boot shows `asqt=0 acqh=0` and `ms=7265`, the
other `asqt=1 acqh=1` and `ms=32806`, all before the GPU probe), the kernel's
LAPIC-calibration digits, `[BLKTOKW]` counters, and **one channel-probe word**
in its own EMEM window (`[NV-GB202FSP] shape off=0x8F2A48 v=0x40` in `rm63` vs
`0x42` in `rm64`) — that block is what the probe itself wrote before the COT,
not a COT input, and the FSP's own scratch (`0x8F0320..0x8F032C`) is identical
in both. The COT's three sysmem addresses, the descriptor's `bootimg`
address/size and the whole `wprm dwords`/`wprm-after dwords` block are
**identical** between the two boots.

The format fix from §11's review is visible here: `wprm dwords`/`wprm-after
dwords` now print `d0=0x…` once (`uart_print_hex` already emits `0x`). The
`rm63`/`rm63b` logs keep the old `0x0x` text — those are historical records and
are quoted as they are; only the two `wprm` rows were changed, so the rest of the
dword rows (`rmargs`, `boot_args`, `frts`) still carry the double prefix.

### 12.2 The rows

```
[NV-FSPRM] start gsp=rm mode=64[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63/64: FMC615.BIN,BOOT615.BIN)
fmc image bytes=287304 fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
bootimg bytes=290816[NV-FSPCOT]  file=BOOT615.BIN d0=0x000000001156E500 d1=0x000000003996718D d2=0x0000000045AF2B00 d3=0x00000000C74FBC4D code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000300000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] gsp-shape post words=64 not-locked=64 transitions=11 first=0x00000000BADF5620 verdict=window-decodes
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=64 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000824 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000012000824 radix3=1
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
```

(The `t_ms=55000` row is abridged with `…` in the middle — the elided fields are
identical to the `t_ms=150` row above it; all 13 rows were parsed and compared
programmatically, and the full row is in the log. Every other quoted row is
verbatim, prefix quirks included.)

Boot bookkeeping from the harness log header and the closing state:

```
# [NV-GB202FSP] boot 2026-11-27T18:04:20Z
# image=/srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_a769beaa/rm64.img sha256=0c84359fae405eacfe7056ba4f28536ece61732aceaa23f12d883a9650784fb4
# tag=rm64 vm=313 hostpci0=0000:41:00
```

Closing state, read on r1 after the boot: `qm status 313` = `stopped`,
`qm status 300` = `stopped`, `pgrep -c qemu-system-x86` = **0**; the guest's
`serial.log` was last written 2026-11-27 12:04:36 -0600 (= 18:04:36Z) and no
boot has touched it since. Staged kernel sha256
`3970903eff220231a8aec61e7894292765827689afc073bf99c9e4dcb28418dc`; the two
firmware files inside the image were re-read with `mtype | sha256sum` and match
§11.1 byte for byte (`ae4e7055…`, `37159b4b…`).

### 12.3 What this settles

**The vendor's own 615-stock `nonWprHeapSize` is inert for the `0x824` abort.**
With every other input identical — same FMC image, same boot image, same COT,
same boot-args page, same heap size, same sampled registers — 1 MiB more
non-WPR heap moves `MAILBOX0` not at all (`0x824` at 150 ms, constant to 55 s),
moves the formatted code not at all (`0x14DC = 0x12000824`), and does not even
arm WPR2 (`0x7FFFFE00/0`). The ACR never laid out WPR2, so a heap *offset* it
would place inside WPR2 cannot be what it validated.

With `frtsSize` demoted on the corpus's own measurement (§11.6) and
`nonWprHeapSize` now measured inert, **every `WprMeta` field in which our block
differs from the vendor's 615 stock has been answered**: the `0x824` abort is
upstream of `WprMeta` *content* on the 287 KiB FMC class. That is consistent
with §11.3's shape (WPR2 never armed; the 570 class arms it and aborts later at
`0x0B`) and with §9's ruled-out table: what is left is either the host↔FMC
input at a layer below the descriptor (the RPC/`GSP_FMC_BOOT_PARAMS` path the
corpus does not publish) or something the FMC reads that only a *different
generation* supplies.

### 12.4 Acceptance

**Unmet, and nothing here is a mock or a shim value.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0`, `[NV-RPC] peer … (header still zero)` at all 13 samples of this
boot, and the status queue's producer header 0. The message id exercised in this
boot is still only the boot channel's: NVDM COT `class=1`, `cmd_nvdm=20 (0x14)`,
answered `errorCode=0` — no `NV_VGPU_MSG_*` request exists to send, because no
RM task has started. Part: **GB202, `41:00.0`, VM 313** in every row.

### 12.5 The named next step (superseded by §13)

The version of this paragraph that shipped in the mode-64 handoff named
**580.65.06** as the next boot, described as "FMC `c62856e9…` (215,624 B,
`code 0xF200`) … the last 215 KiB / first 0xF200-code generation". That naming
was wrong three ways and is withdrawn:

1. it **contradicted this document's own ranking** — the pre-§13 §6 listed
   580.65.06 under "no question left" while rank 2 named the staged `580.178.04`
   pair, which is the same 215 KiB class with the same `0x25800` code size;
2. `code 0xF200` is the **boot image's** `monitorCodeOffset`; the FMC's own
   header field (`+0x0C`) is `0x25800`, and its total extent (`+4`) is `0x34A00`;
3. "the last 215 KiB generation" is false — `580.178.04` is **later** than
   `580.65.06` and the same size, and it is the image the 580 open module binds
   for this part.

The pair the reconciled text names is therefore **`580.178.04`** (FMC
`cea2190c…` container `FMCV.BIN` 216,204 B, with `BOOTIMG.BIN` `ac068832…`,
217,088 B — already staged, and its own same-tag boot image). §13 spends it.

After §13 the next named step is **590.48.01** (FMC `6252aafc…`, 286,792 B,
`0x36E00`, with its own `GspRmBoot` `e1265e10…`), the only image class between
the two measured ends — §6 rank 1, §7.1.

The VM 301 / `nvidia-open-580.178.04` control still needs coordinator sign-off
(§7.2) and was **not** run under this card.

---

## 13. The 580.178.04 boot — NVIDIA's own FMC for this driver, measured (1 boot, 2026-11-27)

This is the boot §12.5's reconciled text names (§6 rank 2 of the survey
version): **the FMC image is the only variable.** Mode 65 = mode 58's host state
(vendor heap pair `gspFwHeapSize 0x8700000` / `nonWprHeapSize 0x220000`, COT
`frtsVidmemOffset 0x4000000` / `frtsVidmemSize 0x100000`, descriptor
`frts_size 0`), with the FMC container swapped from linux-firmware's 570.144
(`FMC.BIN`, image 199,240 B) to 580.178.04's own (`FMCV.BIN`, image 215,624 B).
The **boot image is the same file** (`BOOTIMG.BIN`, 217,088 B `ac068832…`), the
descriptor's `bootloader*` offsets are the same **and correct for a 580 FMC**
(`0xF200`/`0xA00`/`0`, code size `0x25800` = the FMC header's `+0x0C`, §2.4), and
the boot-args page, `WprMeta` field values, GSP-RM image and signature are
identical. So this pair differs from the wall by exactly one artifact: the FMC.

One VM 313 boot, `tools/nv/gb202fsp-boot.sh`, tag `rm65`,
`STOP_AFTER_VERDICT=1 STOP_PATTERN='^\[NV-FSPRM\] verdict='`. Kernel mode 65;
source diff attached (`nv_fsp_cot.c.mode65.diff`).

### 13.1 What was staged

| file | bytes | sha256 | identity |
|---|---|---|---|
| `FMC580C.BIN` (staged from `fw/FMCV.BIN`) | 216,204 | `6adbd92d63c375aa3dc40ea11af35cbc4a03957e3b2c9ae3e778cc5d90ab3cbc` | 580.178.04 container; `image` = **215,624 B, sha256 `cea2190c…`**; header `+4` = `0x34A00`, `+0x0C` = `0x25800` |
| `BOOTIMG.BIN` | 217,088 | `ac0688326c1ea8aae2da26d785e7091bedb9a45565b03543a479ed05db0ba367` | the 580.178.04 `GspRmBoot_GB202` image — **the same file every wall boot staged** |
| `GSPRM.BIN` / `GSPRM.SIG` / `FMC.BIN` / `FMCV.BIN` | 63,541,248 / 4,096 / 199,820 / 216,204 | unchanged from `base.img` | the same GSP-RM image, radix3 source and signature as the `0x0B` boots |

The staged bytes were verified inside the image (`mtype | sha256sum`) before the
boot and the log re-reads them (`fmc image bytes=215624`,
`img_d1=0x0000000000034A00`; `bootimg bytes=217088 … file=BOOTIMG.BIN
code_off=0x000000000000F200`).

### 13.2 The rows

```
[NV-FSPRM] start gsp=rm mode=65[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63/64: FMC615.BIN,BOOT615.BIN; mode 65: FMC580C.BIN,BOOTIMG.BIN)
fmc image bytes=215624 fmc img_d0=0x0000000000480403 img_d1=0x0000000000034A00
bootimg bytes=217088[NV-FSPCOT]  file=BOOTIMG.BIN d0=0x000000004DC21E2B d1=0x000000007D12A87C d2=0x00000000499B881A d3=0x00000000117369FA code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000220000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] cot gspFmcSysmemOffset=0x000000020C616000 frtsSysmemOffset=0x000000020C64B000 gspBootArgsSysmemOffset=0x000000020C74B000 bytes=0x000000000000035C
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000000000000 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000000 partition=0x000000000000000F sk_err=0x0000000000000000 sk_phase=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=27000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
[NV-FSPCOT] gsp-shape post words=64 not-locked=64 transitions=11 first=0x00000000BADF5620 verdict=window-decodes
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=65 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x000000000000000B m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000000000000 radix3=1
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000000000000 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000000 partition=0x000000000000000F sk_err=0x0000000000000000 sk_phase=0x0000000000000000
```

The `rm tl` rows are abridged with `…` in the middle; all 13 samples were parsed
programmatically and every elided field is single-valued in the boot (see
`KEY-ROWS-65.txt`, generated by `keyrows65.py` from the log). The full rows are
in `5090-gb202fsp-rm65.log`.

Boot bookkeeping from the harness header and the closing state:

```
# [NV-GB202FSP] boot 2026-11-27T18:17:54Z
# image=/home/nvcard/.hermes/kanban/boards/hobbyos-nv/workspaces/t_a769beaa/rm65.img sha256=33fac03c922c3dc083a96e549bee9f4f9589097e3df88616bf825e0a9200b8b0
# tag=rm65 vm=313 hostpci0=0000:41:00
```

Closing state, read on r1 after the boot: `qm status 313` = `stopped`,
`qm status 300` = `stopped`, `pgrep -c qemu-system-x86` = **0**; the guest's
`serial.log` was last written 2026-11-27 12:18:10 -0600 (= 18:18:10Z) and no
boot has touched it since. No `disk.img.pre-*` backup was taken for this tag, so
the r1 footprint is one 1 GiB image copy (53 GB free before and after).

### 13.3 What this settles: `0x824` is the 286/287 KiB class; `0x0B` is what a 215 KiB FMC does here

**NVIDIA's own FMC for the 580 driver — the image `nvidia-open-580.178.04`
binds for GB202 — arms WPR2 and then terminates exactly like the 570.144 FMC.**
Three things are identical to the mode-58 control (`fh58`, 570.144 FMC, same
host state) and are the reason this is a one-variable result rather than a
coincidence:

| | `fh58` (570.144 FMC) | `rm65` (580.178.04 FMC) |
|---|---|---|
| `fmc image bytes` | 199,240 | **215,624** |
| `wprm sizes` | heap `0x8700000`, nonwpr `0x220000`, frts `0` | **identical** |
| PFB WPR2 while silent | `0x7E5A200` / `0x7F1EE00` | **identical** |
| terminal `MAILBOX0` | `0x0B` | **`0x0B`** |
| WPR2 at that sample | `0x7FFFFE00` / `0` | **identical** |
| `0x14DC` | `0` | **`0`** |
| `statq_written`/`linked`/`initdone` | `0/0/0` | **`0/0/0`** |

The only differences are the FMC image and the time the attempt ends
(`fh58`: `0x0B` by the 55 s sample, silent at 40 s; `rm65`: `0x0B` by 40 s,
silent at 27 s) — both inside the `0.2–55 s` window `t_0936a17f` measured for
this terminal code.

What follows:

1. **`0x824` is not "the FMC NVIDIA ships".** The 580 image is shipped by
   NVIDIA for this part and answers `0x0B`; `0x824` is confined to the
   286/287 KiB images (590, 595, 610, 615, 620 — 615 and 620 measured).
2. **The wall is not a 570-linux-firmware artifact.** It survives the swap to
   NVIDIA's own prod FMC of a later driver generation with the same host state,
   the same boot image and the same descriptor fields.
3. **The 215 KiB class still arms WPR2 and still cannot finish.** The FSP sizes
   WPR2 from the descriptor exactly as `t_7178dca2` measured (the pair is
   identical to `fh58`'s, so the descriptor reached the FSP), and the ACR still
   writes nothing into the WprMeta block (`untouched=yes`) before ending.
4. **The class boundary is now 580→590**, not 615→620: the only unmeasured image
   class between a measured `0x0B` and a measured `0x824` is `590.48.01`
   (286,792 B, `0x36E00`) — §6 rank 1, §7.1.

### 13.4 Acceptance

**Unmet, and nothing here is a mock or a shim value.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0` in this boot, `[NV-RPC] peer link linked=0 peer=none (status header
still zero: no RM task has started)` at every sample, and `wprm-after
untouched=yes` — no ACR-owned field was ever written, so no allocation object
exists to quote. The message id exercised in this boot is still only the boot
channel's: NVDM COT `class=1`, `cmd_nvdm=20 (0x14)`, answered `errorCode=0` — no
`NV_VGPU_MSG_*` request exists to send, because no RM task has started. Part:
**GB202, `41:00.0`, VM 313** in every row.

### 13.5 The named next step (not spent)

**`590.48.01`** — FMC `6252aafc…` (286,792 B, header `+4` `0x46000`, `+0x0C`
`0x36E00`) with its own `GspRmBoot` `e1265e10…` (286,720 B, `0xF200`/`0x36E00`),
staged as `/NVIDIA/GSP/FMC590.BIN` + `BOOT590.BIN` with a mode that repeats mode
65 and changes only `bl_name`/`bl_code`/`bl_want`. It is the one remaining class
point: `0x824` there puts the abort at the first `0x36E00` image, `0x0B` there
puts it inside the 286→287 KiB step and hands the question to 610.57.04. One
boot, the same harness, the same COT.

The VM 301 / `nvidia-open-580.178.04` control (§7.2) still needs coordinator
sign-off and was **not** run under this card.
